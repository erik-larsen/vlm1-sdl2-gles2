/*
 * delta module: colour plasma area / logo colour-wash. Port of
 * gpu/delta.gas (line references throughout).
 *
 * Submodes: 0 (logo plasma, trigger-gated), 1 (logo with fixed chroma
 * and an accelerating intensity ramp — unused by presets), 2 (solid
 * plasma area), 3 (repositions/recolours the Jaguar-logo OP object
 * via beastie 3 + clut_sha — splash bank), 4 (waveform-textured
 * plasma). Bitmap rows are consumed contiguously at (w+1) longs per
 * row, h+1 rows (the header words are minus-one counts).
 */
#include "fx.h"
#include "banks.h"
#include "waves.h"
#include "logos_data.h"

#include <stddef.h>

/* Wide 21-bit triangle fold (delta.gas:135-153): (v & $1FFFFF) -
 * $100000, absolute value. Result 0..$100000. */
static uint32_t fold21(uint32_t v)
{
    int32_t f = (int32_t)(v & 0x1FFFFF) - 0x100000;
    return (uint32_t)(f < 0 ? -f : f);
}

/* Trigger strobe for the XOR flash (delta.gas:306-318): the toggle
 * byte of `height` (trig) masked by _mtrig. */
static uint32_t xor_strobe(const uint8_t *fx)
{
    uint32_t toggles = vlm_be32(fx + FX_HEIGHT) >> 8;
    uint32_t mtrig = vlm_be32(fx + FX_MTRIG);
    return mtrig ? (toggles & mtrig) : 0;
}

typedef struct {
    uint32_t colx, coly;      /* r7/r8 colour phases */
    int32_t cvx, cvy;         /* per-pixel steps (>>2) */
    int32_t cvx2, cvy2;       /* per-row steps (>>4) */
    int32_t x0, y0, w, h;     /* dest rect */
    uint32_t strobe;
} Plasma;

static void plasma_common(const uint8_t *fx, Plasma *p,
                          int32_t w, int32_t h)
{
    p->w = w;
    p->h = h;
    p->x0 = ((int32_t)vlm_be32(fx + FX_DSTOFFX) >> 16) - (w >> 1);
    p->y0 = ((int32_t)vlm_be32(fx + FX_DSTOFFY) >> 16) - (h >> 1);
    p->cvx = (int32_t)vlm_be32(fx + FX_CVX) >> 2;
    p->cvy = (int32_t)vlm_be32(fx + FX_CVY) >> 2;
    p->cvx2 = (int32_t)vlm_be32(fx + FX_CVX2) >> 4;
    p->cvy2 = (int32_t)vlm_be32(fx + FX_CVY2) >> 4;
    p->colx = vlm_be32(fx + FX_COLX);
    p->coly = vlm_be32(fx + FX_COLY);
    p->strobe = xor_strobe(fx);
}

/* The original writes with no bounds checks (the dest walks off the
 * buffer if the rect leaves the screen); we clamp per pixel to stay
 * memory-safe while keeping the phase stepping identical. */
static void plasma_store(VlmFxCtx *c, int32_t x, int32_t y, uint16_t v)
{
    if (x >= 0 && x < VLM_SCREEN_W && y >= 0 && y < VLM_SCREEN_H)
        c->screen[y * VLM_SCREEN_W + x] = v;
}

/* Submode 2 — solid plasma area (delta.gas:269-376). */
static void delta_plasma(VlmFxCtx *c)
{
    const uint8_t *fx = c->fx;
    Plasma p;
    plasma_common(fx, &p,
                  (int32_t)(vlm_be32(fx + FX_J) >> 8),
                  (int32_t)(vlm_be32(fx + FX_K) >> 8));

    for (int32_t row = 0; row <= p.h; row++) {
        uint32_t px = p.colx, py = p.coly;
        for (int32_t col = 0; col <= p.w; col++) {
            uint32_t a = fold21(px), b = fold21(py);
            uint32_t cc = ((a >> 16) << 4) | (b >> 16);
            if (p.strobe)
                cc ^= 0xFF;
            uint32_t yv = (b >> 13) + 64;
            plasma_store(c, p.x0 + col, p.y0 + row,
                         (uint16_t)((cc << 8) | yv));
            px += (uint32_t)p.cvx;
            py += (uint32_t)p.cvy;
        }
        p.colx += (uint32_t)p.cvx2;
        p.coly += (uint32_t)p.cvy2;
    }
}

/* Submode 4 — waveform-textured plasma (delta.gas:438-554): chroma
 * nibbles come from sine_bas table lookups of the folded phases. */
static void delta_waveplasma(VlmFxCtx *c)
{
    const uint8_t *fx = c->fx;
    const uint8_t *tab = vlm_wave_table_from_addr(vlm_be32(fx + FX_SINE_BAS));
    Plasma p;
    plasma_common(fx, &p,
                  (int32_t)(vlm_be32(fx + FX_J) >> 7),
                  (int32_t)(vlm_be32(fx + FX_K) >> 8));

    for (int32_t row = 0; row <= p.h; row++) {
        uint32_t px = p.colx, py = p.coly;
        for (int32_t col = 0; col <= p.w; col++) {
            uint32_t a = fold21(px), b = fold21(py);
            uint32_t cc = (uint32_t)((tab[a >> 12] >> 4) << 4)
                          | (tab[b >> 12] >> 4);
            if (p.strobe)
                cc ^= 0xFF;
            uint32_t yv = (b >> 13) + 64;
            plasma_store(c, p.x0 + col, p.y0 + row,
                         (uint16_t)((cc << 8) | yv));
            px += (uint32_t)p.cvx;
            py += (uint32_t)p.cvy;
        }
        p.colx += (uint32_t)p.cvx2;
        p.coly += (uint32_t)p.cvy2;
    }
}

/* bmobj table (vlm.s:8344): jlogo2, jaglogo, blokk, blokk. blokk is
 * a 4-byte header into unrelated ROM data — treated as absent. */
static const uint8_t *bmobj(const uint8_t *fx)
{
    switch ((vlm_be32(fx + FX_THANG) >> 16) & 3) {
    case 0: return vlm_jlogo2;
    case 1: return vlm_jaglogo;
    default: return NULL;
    }
}

/* Submode 0 — logo colour-wash plasma (delta.gas:111-190). */
static void delta_logo(VlmFxCtx *c)
{
    const uint8_t *fx = c->fx;
    const uint8_t *bmp = bmobj(fx);
    if (!bmp)
        return;
    /* trigger gate: all _mtrig bits must be set (delta.gas:111-123) */
    uint32_t trig = vlm_be32(fx + FX_HEIGHT);
    uint32_t mtrig = vlm_be32(fx + FX_MTRIG);
    if (mtrig && (trig & mtrig) != mtrig)
        return;

    int32_t w = (int32_t)vlm_be16(bmp);
    int32_t h = (int32_t)vlm_be16(bmp + 2);
    const uint8_t *src = bmp + 4;
    int32_t x0 = ((int32_t)vlm_be32(fx + FX_DSTOFFX) >> 16) - (w << 4);
    int32_t y0 = ((int32_t)vlm_be32(fx + FX_DSTOFFY) >> 16) - (h >> 1);
    int32_t cvx = (int32_t)vlm_be32(fx + FX_CVX) >> 2;
    int32_t cvy = (int32_t)vlm_be32(fx + FX_CVY) >> 2;
    int32_t cvx2 = (int32_t)vlm_be32(fx + FX_CVX2) >> 4;
    int32_t cvy2 = (int32_t)vlm_be32(fx + FX_CVY2) >> 4;
    uint32_t colx = vlm_be32(fx + FX_COLX);
    uint32_t coly = vlm_be32(fx + FX_COLY);

    for (int32_t row = 0; row <= h; row++) {
        uint32_t px = colx, py = coly;
        int32_t col = 0;
        for (int32_t lng = 0; lng <= w; lng++) {
            uint32_t bits = vlm_be32(src);
            src += 4;
            for (int b = 0; b < 32; b++) {
                if (bits & 0x80000000u) {
                    uint32_t a = fold21(px), bb = fold21(py);
                    uint32_t cc = ((a >> 16) << 4) | (bb >> 16);
                    uint32_t yv = (bb >> 13) + 64;
                    plasma_store(c, x0 + col, y0 + row,
                                 (uint16_t)((cc << 8) | yv));
                }
                bits <<= 1;
                px += (uint32_t)cvx;
                py += (uint32_t)cvy;
                col++;
            }
        }
        colx += (uint32_t)cvx2;
        coly += (uint32_t)cvy2;
    }
}

/* Submode 1 — logo, fixed chroma, accelerating intensity ramp
 * (delta.gas:191-268). Unused by presets but cheap to keep exact. */
static void delta_logo_ramp(VlmFxCtx *c)
{
    const uint8_t *fx = c->fx;
    const uint8_t *bmp = bmobj(fx);
    if (!bmp)
        return;

    int32_t w = (int32_t)vlm_be16(bmp);
    int32_t h = (int32_t)vlm_be16(bmp + 2);
    const uint8_t *src = bmp + 4;
    int32_t x0 = ((int32_t)vlm_be32(fx + FX_DSTOFFX) >> 16) - (w << 4);
    int32_t y0 = ((int32_t)vlm_be32(fx + FX_DSTOFFY) >> 16) - (h >> 1);
    uint32_t cc = vlm_fx_fold(vlm_be32(fx + FX_COLX),
                              vlm_be32(fx + FX_COLY));
    int32_t pstep_inc = (int32_t)vlm_be32(fx + FX_CVY) >> 2;
    int32_t rstep_inc = (int32_t)vlm_be32(fx + FX_CVY2) >> 4;
    int32_t rstep = (int32_t)vlm_be32(fx + FX_CVX2);
    uint32_t arow = vlm_be32(fx + FX_I1) << 8;
    int32_t step0 = (int32_t)vlm_be32(fx + FX_CVX);

    for (int32_t row = 0; row <= h; row++) {
        uint32_t a = arow;
        int32_t step = step0;
        int32_t col = 0;
        for (int32_t lng = 0; lng <= w; lng++) {
            uint32_t bits = vlm_be32(src);
            src += 4;
            for (int b = 0; b < 32; b++) {
                if (bits & 0x80000000u) {
                    int32_t f = (int32_t)(a & 0x1FFFFE) - 0xFFFFF;
                    if (f < 0)
                        f = -f;
                    uint32_t yv = ((uint32_t)f >> 17) + 64;
                    plasma_store(c, x0 + col, y0 + row,
                                 (uint16_t)(cc | yv));
                }
                bits <<= 1;
                step += pstep_inc;
                a += (uint32_t)step;
                col++;
            }
        }
        rstep += rstep_inc;
        arow += (uint32_t)rstep;
    }
}

/* Submode 3 — reposition/recolour the Jaguar-logo display object
 * (delta.gas:377-434): writes beastie 3's X/Y/mode and the clut_sha
 * CRY word the Frame ISR feeds to the logo's CLUT entry. */
static void delta_recolour(VlmFxCtx *c)
{
    const uint8_t *fx = c->fx;
    c->logo_active = 1;
    c->logo_x = (int32_t)vlm_be32(fx + FX_DSTOFFX);
    c->logo_y = (int32_t)(vlm_be32(fx + FX_DY) << 1);
    uint32_t a = fold21(vlm_be32(fx + FX_COLX));
    uint32_t b = fold21(vlm_be32(fx + FX_COLY));
    c->clut_sha = (uint16_t)(((((a >> 16) << 4) | (b >> 16)) << 8)
                             | (vlm_be32(fx + FX_I1) >> 8));
}

void vlm_fx_delta(VlmFxCtx *c)
{
    switch (c->submode) {
    case 0:
        delta_logo(c);
        break;
    case 2:
        delta_plasma(c);
        break;
    case 3:
        delta_recolour(c);
        break;
    case 4:
        delta_waveplasma(c);
        break;
    default:
        delta_logo_ramp(c); /* submode 1 / anything else: default path */
        break;
    }
}
