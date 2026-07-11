/*
 * Shared effect helpers: CRY colour fold, waveform lookup, plot
 * routines, mirror-symmetry chain, and the rotational-symmetry engine.
 * Faithful ports of the code shared by alpha/epsilon/sigma/shu
 * (references are to gpu/alpha.gas, the canonical copy).
 */
#include "fx.h"
#include "banks.h"
#include "waves.h"

uint32_t vlm_fx_fold(uint32_t colx, uint32_t coly)
{
    /* alpha.gas:86-107: triangle fold of each phase to 0..15, then
     * CC = (fx<<4)|fy, returned as the pixel high byte (CC<<8). */
    int32_t fx = (int32_t)((colx >> 16) & 0x1E) - 15;
    int32_t fy = (int32_t)((coly >> 16) & 0x1E) - 15;
    if (fx < 0)
        fx = -fx;
    if (fy < 0)
        fy = -fy;
    return (uint32_t)(((fx << 4) | fy) << 8);
}

int32_t vlm_fx_wave_signed(const uint8_t *tab, uint32_t phase)
{
    uint32_t idx = (phase >> 16) & 0xFF;
    int32_t s0 = tab[idx], s1 = tab[idx + 1];
    int32_t frac = (int32_t)((phase >> 8) & 0xFF);
    return ((s0 - 128) << 8) + (s1 - s0) * frac;
}

int32_t vlm_fx_wave_unsigned(const uint8_t *tab, uint32_t phase)
{
    uint32_t idx = (phase >> 16) & 0xFF;
    int32_t s0 = tab[idx], s1 = tab[idx + 1];
    int32_t frac = (int32_t)((phase >> 8) & 0xFF);
    return (s0 << 8) + (s1 - s0) * frac;
}

/* ---- plot routines (alpha.gas:627-671, 906-983, 1016-1022) --------- */

static void plot_1x1(VlmFxCtx *c, int32_t x, int32_t y)
{
    if (x >= VLM_CLIP_W || y >= VLM_CLIP_W || x < 0 || y < 0)
        return;
    uint16_t pix = (uint16_t)(((c->intensity & 0xFFFF) >> 8) | c->cc_pix);
    c->screen[y * VLM_SCREEN_W + x] = pix;
}

static void plot_2x2(VlmFxCtx *c, int32_t x, int32_t y)
{
    if (x >= VLM_CLIP_W || y >= VLM_CLIP_W || x < 0 || y < 0)
        return;
    uint16_t pix = (uint16_t)(((c->intensity & 0xFFFF) >> 8) | c->cc_pix);
    uint16_t *p = c->screen + y * VLM_SCREEN_W + x;
    /* store order: (x,y), (x+1,y), (x+1,y+1), (x,y+1) — unclipped like
     * the original; x,y <= $17D keeps this inside the 384x384 buffer. */
    p[0] = pix;
    p[1] = pix;
    p[VLM_SCREEN_W + 1] = pix;
    p[VLM_SCREEN_W] = pix;
}

/* N x N solid fill via the blitter (alpha.gas:919-983). */
static void plot_nxn(VlmFxCtx *c, int32_t x, int32_t y, int32_t n)
{
    if (n == 1) {
        plot_1x1(c, x, y);
        return;
    }
    if (n == 2) {
        plot_2x2(c, x, y);
        return;
    }
    int32_t lim = VLM_CLIP_W - n;
    int32_t x0 = x - (n >> 1);
    if (x0 < 0 || x0 > lim)
        return;
    int32_t y0 = y - (n >> 1);
    if (y0 < 0 || y0 > lim)
        return;
    uint16_t pix = (uint16_t)(((c->intensity & 0xFFFF) >> 8) | c->cc_pix);
    for (int32_t row = 0; row < n; row++) {
        uint16_t *p = c->screen + (y0 + row) * VLM_SCREEN_W + x0;
        for (int32_t col = 0; col < n; col++)
            p[col] = pix;
    }
}

/* Dispatch on the plot_mod field. The original's 4-entry table at
 * module offset $9B0 is {2x2, 2x2, size-dispatch, intensity-scaled
 * size-dispatch} (verified against vlm.abs). */
static void plot_point(VlmFxCtx *c, int32_t x, int32_t y)
{
    switch (c->plot_mode & 3) {
    case 0:
    case 1:
        plot_2x2(c, x, y);
        break;
    case 2:
        plot_nxn(c, x, y,
                 (int32_t)(vlm_be32(c->fx + FX_PIXSIZE) >> 16) + 1);
        break;
    case 3: {
        /* alpha.gas:1016-1022: N = ((intensity * pixscale) &
         * $7F0000)>>16 + 1 — dot size scales with brightness. */
        uint32_t v = (vlm_fx_mult(c->intensity, c->pixscale)
                      & 0x7F0000) >> 16;
        plot_nxn(c, x, y, (int32_t)v + 1);
        break;
    }
    }
}

/* ---- mirror-symmetry chain (alpha.gas:566-623) ---------------------- */

static void mirror_chain(VlmFxCtx *c, int32_t x, int32_t y)
{
    uint32_t bits = c->asym;
    const int32_t W = VLM_CLIP_W;
    for (;;) {
        if (bits & 1)
            plot_point(c, x, y);
        x = W - x;
        if (bits & 2)
            plot_point(c, x, y);
        y = W - y;
        if (bits & 4)
            plot_point(c, x, y);
        x = W - x;
        if (bits & 8)
            plot_point(c, x, y);
        /* step D (alpha.gas:606-622): diagonal swap, shift to the
         * next bit nibble; done when the shifted bit4 is set (bit8
         * marks the second pass) */
        int32_t t = W - x;
        x = y;
        y = t;
        bits >>= 4;
        if (bits & 0x10)
            break;
        bits |= 0x100;
    }
}

/* ---- rotational-symmetry engine (alpha.gas:386-564) ----------------- */

static void rot_engine(VlmFxCtx *c, int32_t x, int32_t y)
{
    const uint8_t *fx = c->fx;
    uint32_t saved_cc = c->cc_pix;

    /* Per-copy colour phases seeded from the current CC nibbles. */
    uint32_t cc = saved_cc >> 8;
    uint32_t colb = ((cc >> 4) & 0xF) << 16;
    uint32_t colc = (cc & 0xF) << 16;

    int32_t count = (int32_t)vlm_be32(fx + FX_RSYM_ORD);
    uint32_t step = vlm_be32(fx + FX_RSYM_STE);
    uint32_t ist = vlm_be32(fx + FX_RSYM_IST);
    uint32_t accum = vlm_be32(fx + FX_RSYM_PHASE);
    const uint8_t *tab = vlm_wave_table_from_addr(vlm_be32(fx + FX_SINE_BAS));

    uint32_t rx = vlm_be32(fx + FX_RXCEN);
    uint32_t ry = vlm_be32(fx + FX_RYCEN);
    int32_t dx0 = x - ((int32_t)rx >> 16);
    int32_t dy0 = y - ((int32_t)ry >> 16);
    int32_t dx = dx0, dy = dy0;

    for (;;) {
        /* per copy: angle += step, step += step-delta */
        accum += step;
        step += ist;

        uint32_t idx = (accum >> 16) & 0xFF;
        uint32_t frac = (accum >> 8) & 0xFF;
        int32_t s0 = tab[idx], s1 = tab[idx + 1];
        int32_t sinv = ((s0 - 128) << 8) + (s1 - s0) * (int32_t)frac;
        uint32_t ci = (idx + 64) & 0xFF;
        int32_t c0 = tab[ci], c1 = tab[ci + 1];
        int32_t cosv = ((c0 - 128) << 8) + (c1 - c0) * (int32_t)frac;

        int32_t px = (vlm_fx_imult((uint32_t)cosv, (uint32_t)dx)
                      - vlm_fx_imult((uint32_t)sinv, (uint32_t)dy)) >> 15;
        int32_t py = (vlm_fx_imult((uint32_t)cosv, (uint32_t)dy)
                      + vlm_fx_imult((uint32_t)sinv, (uint32_t)dx)) >> 15;

        uint32_t roscale = vlm_be32(fx + FX_ROSCALE);
        px = vlm_fx_imult(roscale, (uint32_t)px) >> 12;
        py = vlm_fx_imult(roscale, (uint32_t)py) >> 12;
        px += (int32_t)rx >> 16;
        py += (int32_t)ry >> 16;

        /* per-copy colour: fold then advance by cvx/cvy */
        c->cc_pix = vlm_fx_fold(colb, colc);
        colb += vlm_be32(fx + FX_CVX);
        colc += vlm_be32(fx + FX_CVY);

        mirror_chain(c, px, py);

        /* per-copy continue (alpha.gas:527-553): re-derive dx/dy from
         * the ORIGINAL offsets each copy (not compounding), drift the
         * centre by drxcen/drycen. */
        dx = vlm_fx_imult(vlm_be32(fx + FX_ROSCAL2) + 0x1000,
                          (uint32_t)dx0) >> 12;
        dy = vlm_fx_imult(vlm_be32(fx + FX_ROSCALEI) + 0x1000,
                          (uint32_t)dy0) >> 12;
        rx += vlm_be32(fx + FX_DRXCEN);
        ry += vlm_be32(fx + FX_DRYCEN);

        if (--count < 0)
            break;
    }
    c->cc_pix = saved_cc;
}

void vlm_fx_symmetry_plot(VlmFxCtx *c, int32_t x, int32_t y)
{
    if (c->asym & 0x10000)
        rot_engine(c, x, y);
    else
        mirror_chain(c, x, y);
}
