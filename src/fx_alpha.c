/*
 * alpha module: starfield (submode 0), sine trail (2), ring of pixels
 * (9). Port of gpu/alpha.gas — comments reference its line numbers.
 * Submode 1 jumps into the symmetry chain with indeterminate register
 * state on hardware and is unused by all shipped presets; it is a
 * no-op here.
 */
#include "fx.h"
#include "banks.h"
#include "waves.h"

/* 16-bit-tap LFSR step (alpha.gas:147-167): shift, xor $3500 if the
 * dropped bit was set. */
static uint32_t lfsr_step(uint32_t v)
{
    uint32_t bit = v & 1;
    v >>= 1;
    if (bit)
        v ^= 0x3500;
    return v;
}

/* Perspective + depth brightness + plot (alpha.gas:985-1014).
 * Starfield enters with the <<14 pre-shift; trail enters 4 bytes in
 * ($F03948) with x/y already 16.16. */
static void perspective_plot(VlmFxCtx *c, int32_t x, int32_t y,
                             uint32_t z, int shift14)
{
    if (shift14) {
        x <<= 14;
        y <<= 14;
    }
    z &= 0x1FF;
    c->intensity = (0x1FF - z) << 7;
    uint32_t d = z << 2;
    x = vlm_fx_sdiv(x, d);
    y = vlm_fx_sdiv(y, d);
    x = (x >> 5) + 0xC0;
    y = (y >> 5) + 0xC0;
    vlm_fx_symmetry_plot(c, x, y);
}

/* Submode 0 — starfield (alpha.gas:80-191). */
static void alpha_starfield(VlmFxCtx *c)
{
    const uint8_t *fx = c->fx;

    c->cc_pix = vlm_fx_fold(vlm_be32(fx + FX_COLX), vlm_be32(fx + FX_COLY));
    c->pixscale = vlm_be32(fx + FX_PIXSIZE) >> 16;

    int32_t ox = (int32_t)vlm_be32(fx + FX_DSTOFFX) >> 12;
    int32_t oy = (int32_t)vlm_be32(fx + FX_DSTOFFY) >> 12;
    int32_t oz = (int32_t)vlm_be32(fx + FX_DSTOFFZ) >> 12;

    /* LFSRs reseed identically each frame; motion comes from dstoff*
     * advancing under the modulation matrix. */
    uint32_t la = 8;
    uint32_t lb = vlm_be32(fx + FX_J);
    uint32_t lc = 0xF;

    int32_t n = (int32_t)(vlm_be32(fx + FX_ITER) >> 16);
    do {
        la = lfsr_step(la);
        lb = lfsr_step(lb);
        lc = lfsr_step(lc);
        int32_t x = (int32_t)((la + ((uint32_t)ox >> 4)) & 0x1FF) - 0xFF;
        int32_t y = (int32_t)((lb + ((uint32_t)oy >> 4)) & 0x1FF) - 0xFF;
        uint32_t z = (lc + ((uint32_t)oz >> 4)) & 0x1FF;
        perspective_plot(c, x, y, z, 1);
    } while (--n >= 0);
}

/* Submode 2 — sine trail (alpha.gas:192-371). */
static void alpha_trail(VlmFxCtx *c)
{
    uint8_t *fx = c->fx;

    c->pixscale = vlm_be32(fx + FX_PIXSIZE) >> 16;

    int32_t x = (int32_t)vlm_be32(fx + FX_DSTOFFX) - 0xC00000;
    int32_t y0 = (int32_t)vlm_be32(fx + FX_DSTOFFY) - 0xC00000;
    int32_t z = (int32_t)vlm_be32(fx + FX_DSTOFFZ);

    int32_t n = (int32_t)(vlm_be32(fx + FX_ITER) >> 16);
    int32_t count2 = (int32_t)(vlm_be32(fx + FX_ITER2) >> 16);
    int32_t vx = (int32_t)vlm_be32(fx + FX_VECTORX);
    int32_t vy = (int32_t)vlm_be32(fx + FX_VECTORY);

    /* centre the trail: x -= fmult(vectorx, count<<16)/2 (223-260) */
    x -= vlm_fx_fmult(vx, n << 16) >> 1;

    uint32_t ph2 = vlm_be32(fx + FX_PHASE2);
    uint32_t phase = vlm_be32(fx + FX_PHASE1) + vlm_be32(fx + FX_PHASE5);
    uint32_t phase0 = phase;

    uint32_t colx0 = vlm_be32(fx + FX_COLX);
    uint32_t colx = colx0;
    uint32_t coly = vlm_be32(fx + FX_COLY) + vlm_be32(fx + FX_CVY2);

    uint32_t k = vlm_be32(fx + FX_K);
    const uint8_t *tab = vlm_wave_table_from_addr(vlm_be32(fx + FX_SINE_BAS));

    for (;;) {
        c->cc_pix = vlm_fx_fold(colx, coly);
        colx += vlm_be32(fx + FX_CVX2);

        /* wave = (unsigned interp * k.lo16) asr 8 (325-342) */
        int32_t wave = vlm_fx_wave_unsigned(tab, phase);
        wave = (int32_t)vlm_fx_mult(k, (uint32_t)wave) >> 8;

        perspective_plot(c, x, y0 + wave, (uint32_t)z >> 16, 0);

        x += vx;
        phase += ph2;           /* delay slot: happens on both paths */
        if (--n >= 0)
            continue;

        /* segment restart (359-368): z back-steps, phase resets */
        z -= vy;
        phase = phase0;
        if (z < 0)
            break;
        if (--count2 < 0)
            break;
        coly += vlm_be32(fx + FX_CVY2);
        colx = colx0;
    }
}

/* Submode 9 — ring of pixels (alpha.gas:676-905). */
static void alpha_ring(VlmFxCtx *c)
{
    uint8_t *fx = c->fx;

    /* _i1/_i2 >> 12 + 1 act as index frequency multipliers (676-687) */
    uint32_t stepx = (vlm_be32(fx + FX_I1) >> 12) + 1;
    uint32_t stepy = (vlm_be32(fx + FX_I2) >> 12) + 1;

    c->cc_pix = vlm_fx_fold(vlm_be32(fx + FX_COLX), vlm_be32(fx + FX_COLY));
    c->pixscale = vlm_be32(fx + FX_PIXSIZE) >> 16;

    int32_t x0 = (int32_t)vlm_be32(fx + FX_DSTOFFX);
    int32_t y0 = (int32_t)vlm_be32(fx + FX_DSTOFFY);
    uint32_t angle = vlm_be32(fx + FX_PHASE1);
    int32_t count = (int32_t)vlm_be32(fx + FX_ITER) >> 16;
    int32_t radius = (int32_t)vlm_be32(fx + FX_RADIUS);
    uint32_t astep = vlm_be32(fx + FX_PHASE4);

    if (count >= 1 && (radius >> 16) >= 4) {
        c->intensity = 0xFFFF;
        uint32_t ringstep = 0x1000000u / (uint32_t)count;
        const uint8_t *tab =
            vlm_wave_table_from_addr(vlm_be32(fx + FX_SINE_BAS));

        for (int32_t n = count;;) {
            /* two lookups with the index (and fraction) multiplied by
             * stepx/stepy; the second at angle+astep gives the 90°
             * companion (747-853) */
            uint32_t idx = (((angle >> 16) & 0xFFFF) * stepx) & 0xFF;
            uint32_t frac = (((angle >> 8) & 0xFFFF) * stepx) & 0xFF;
            int32_t s0 = tab[idx], s1 = tab[idx + 1];
            int32_t sv = ((s0 - 128) << 8) + (s1 - s0) * (int32_t)frac;
            int32_t dy = vlm_fx_fmult(sv, radius);

            uint32_t a2 = angle + astep;
            uint32_t idx2 = (((a2 >> 16) & 0xFFFF) * stepy) & 0xFF;
            uint32_t frac2 = (((a2 >> 8) & 0xFFFF) * stepy) & 0xFF;
            int32_t t0 = tab[idx2], t1 = tab[idx2 + 1];
            int32_t sv2 = ((t0 - 128) << 8) + (t1 - t0) * (int32_t)frac2;
            int32_t dx = vlm_fx_fmult(sv2, radius);

            /* logical >>16: negative sums leave the clip window */
            int32_t px = (int32_t)((uint32_t)(x0 + dx) >> 16);
            int32_t py = (int32_t)((uint32_t)(y0 + dy) >> 16);
            vlm_fx_symmetry_plot(c, px, py);

            angle += ringstep;  /* delay slot: also on exit */
            if (--n < 0)
                break;
        }
    }

    /* Echo-run self-update (858-902): each replay of a delay-line
     * snapshot grows/shifts it, animating the ghosts. */
    if (c->echo_run) {
        radius += (int32_t)vlm_be32(fx + FX_VECTORY) >> 3;
        vlm_wbe32(fx + FX_RADIUS, (uint32_t)radius);
        vlm_wbe32(fx + FX_COLX,
                  vlm_be32(fx + FX_COLX) + vlm_be32(fx + FX_CVX2));
        vlm_wbe32(fx + FX_COLY,
                  vlm_be32(fx + FX_COLY) + vlm_be32(fx + FX_CVY2));
        vlm_wbe32(fx + FX_PIXSIZE,
                  vlm_be32(fx + FX_PIXSIZE)
                  + ((uint32_t)((int32_t)vlm_be32(fx + FX_VECTORX) >> 7)));
    }
}

void vlm_fx_alpha(VlmFxCtx *c)
{
    c->asym = vlm_be32(c->fx + FX_ASYM_FLA);
    c->plot_mode = (int)vlm_be32(c->fx + FX_PLOT_MOD);

    switch (c->submode) {
    case 1:
        break; /* register-residue mode, unused by presets */
    case 2:
        alpha_trail(c);
        break;
    case 9:
        alpha_ring(c);
        break;
    default:
        alpha_starfield(c);
        break;
    }
}
