/*
 * delta module: colour plasma area / logo colour-wash. Port of
 * gpu/delta.gas (line references throughout).
 *
 * Ported: submode 2 (solid plasma area, delta.gas:269-376) and
 * submode 4 (waveform-textured plasma, delta.gas:438-554) — the two
 * used by presets without external assets.
 * TODO: submode 0/1 (logo plasma; needs the bmobj 1-bit bitmaps from
 * images.s) and submode 3 (recolours a hardware display object via
 * clut_sha; needs the object-processor layer).
 */
#include "fx.h"
#include "banks.h"
#include "waves.h"

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

void vlm_fx_delta(VlmFxCtx *c)
{
    switch (c->submode) {
    case 2:
        delta_plasma(c);
        break;
    case 4:
        delta_waveplasma(c);
        break;
    case 0:  /* TODO: logo plasma (needs bmobj bitmaps) */
    case 3:  /* TODO: object recolour (needs OP layer) */
    default: /* submode 1 unused by presets */
        break;
    }
}
