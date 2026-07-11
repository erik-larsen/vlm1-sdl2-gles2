/*
 * gamma module: beat-gated Gouraud polygon objects. Port of
 * gpu/gamma.gas (line references throughout).
 *
 * Pipeline: for each face of polyos[(thang>>16)&7], transform its 3
 * vertices (deform by j/k-0x8000, rotate by phase1's byte-resolution
 * sin/cos), emit each vertex through the REDUCED symmetry path —
 * gamma ignores the mirror bits entirely; a vertex emits once, or
 * rsym_ord+1 rotational copies (gamma.gas:340-487) — then assemble
 * one triangle per symmetric copy and rasterize with the blitter's
 * Gouraud span semantics: colour byte constant per face, corner
 * intensities pulled live from _i1.._i6 via each vertex's code word.
 *
 * The rasterizer keeps the original's arithmetic: +1-row counts,
 * truncating signed divides for edge/intensity slopes, sign-magnitude
 * fmult bulk advances on Y clips, ymax = $13F, and the 4-pixel
 * phrase-phase pre-step with its overflow-undo quirk
 * (gamma.gas:1084-1105). Narrow spans (<16 px) use the flat
 * left-edge intensity (the hardware path leaves stale Gouraud lanes;
 * flat is the evident intent).
 */
#include "fx.h"
#include "banks.h"
#include "waves.h"
#include "polys_data.h"

#define GAMMA_YMAX 0x13F
#define MAX_COPIES 256

typedef struct {
    int32_t x, y;
    uint16_t code; /* (facecol<<8) | intensity-code */
} GEmit;

/* ---- span fill (gamma.gas:1033-1146) -------------------------------- */

static void gamma_span(VlmFxCtx *c, int32_t y, uint32_t colbyte,
                       int32_t xa, int32_t ia, int32_t xb, int32_t ib)
{
    if (xb < xa) {
        int32_t t = xa;
        xa = xb;
        xb = t;
        t = ia;
        ia = ib;
        ib = t;
    }
    int32_t dx = xb - xa;
    int32_t di = ib - ia;
    int32_t xl = xa >> 16;
    int32_t w = (int32_t)(((uint32_t)dx >> 16) + 1);
    int32_t d = vlm_fx_sdiv(di, (uint32_t)w);

    if (y < 0 || y >= VLM_SCREEN_H)
        return;

    uint16_t *row = c->screen + y * VLM_SCREEN_W;
    if ((w >> 4) != 0) {
        /* Gouraud: pre-step back by the pixel phase so the phrase
         * lanes align to screen x; undo if it overflowed the 24-bit
         * intensity (gamma.gas:1084-1105). */
        int32_t ph = xl & 3;
        int32_t base = ia - ph * d;
        if ((uint32_t)base >> 24)
            base = ia;
        for (int32_t px = 0; px < w; px++) {
            int32_t x = xl + px;
            if (x < 0 || x >= VLM_SCREEN_W)
                continue;
            int32_t iv = base + d * (px + ph);
            if (iv < 0)
                iv = 0;
            if (iv > 0xFFFFFF)
                iv = 0xFFFFFF;
            row[x] = (uint16_t)((colbyte << 8) | ((uint32_t)iv >> 16));
        }
    } else {
        int32_t iv = ia;
        if (iv < 0)
            iv = 0;
        if (iv > 0xFFFFFF)
            iv = 0xFFFFFF;
        uint16_t pix = (uint16_t)((colbyte << 8) | ((uint32_t)iv >> 16));
        for (int32_t px = 0; px < w; px++) {
            int32_t x = xl + px;
            if (x >= 0 && x < VLM_SCREEN_W)
                row[x] = pix;
        }
    }
}

/* ---- triangle rasterizer (gamma.gas:512-1032) ------------------------ */

static void gamma_triangle(VlmFxCtx *c, uint32_t colbyte,
                           int32_t x0, int32_t y0, int32_t i0,
                           int32_t x1, int32_t y1, int32_t i1,
                           int32_t x2, int32_t y2, int32_t i2)
{
    int32_t t;
    /* sort ascending by y, intensities follow (gamma.gas:533-568) */
    if (y1 < y0) {
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
        t = i0; i0 = i1; i1 = t;
    }
    if (y2 < y0) {
        t = x0; x0 = x2; x2 = t;
        t = y0; y0 = y2; y2 = t;
        t = i0; i0 = i2; i2 = t;
    }
    if (y2 < y1) {
        t = x1; x1 = x2; x2 = t;
        t = y1; y1 = y2; y2 = t;
        t = i1; i1 = i2; i2 = t;
    }

    int32_t rows_top = y1 - y0 + 1;
    int32_t rows_bot = y2 - y1 + 1;
    int32_t rows_all = y2 - y0 + 1;

    int32_t dxA = vlm_fx_sdiv((x1 - x0) << 16, (uint32_t)rows_top);
    int32_t dxB = vlm_fx_sdiv((x2 - x0) << 16, (uint32_t)rows_all);
    int32_t diA = vlm_fx_sdiv(i1 - i0, (uint32_t)rows_top);
    int32_t diB = vlm_fx_sdiv(i2 - i0, (uint32_t)rows_all);

    int32_t Ax = x0 << 16, Ai = i0;
    int32_t Bx = x0 << 16, Bi = i0;
    int32_t y = y0;
    int skip_top = 0;

    if (y0 < 0) {
        if (y1 > 0) {
            /* partial top clip (gamma.gas:717-843): bulk-advance both
             * edges by -y0 rows via fmult, adjust the row count with
             * the original's +1 */
            int32_t skip = (-y0) << 16;
            rows_top = rows_top + y0 + 1;
            Ax += vlm_fx_fmult(dxA, skip);
            Ai += vlm_fx_fmult(diA, skip);
            Bx += vlm_fx_fmult(dxB, skip);
            Bi += vlm_fx_fmult(diB, skip);
            y = 0;
        } else if (y1 == 0 || y2 >= 0) {
            /* top half fully above: advance the long edge across it,
             * restart edge A at the middle vertex (gamma.gas:650-716) */
            int32_t skip = rows_top << 16;
            Bx += vlm_fx_fmult(dxB, skip);
            Bi += vlm_fx_fmult(diB, skip);
            Ax = x1 << 16;
            Ai = i1;
            y = 0;
            skip_top = 1;
        } else {
            return; /* entirely above the screen */
        }
    }

    if (!skip_top) {
        /* ymax gate for the top half (gamma.gas:839-853) */
        if (y0 >= GAMMA_YMAX)
            return;
        if (y1 >= GAMMA_YMAX) {
            rows_top -= y1 - GAMMA_YMAX;
            if (rows_top <= 0)
                return;
        }
        for (int32_t r = 0; r < rows_top; r++) {
            gamma_span(c, y, colbyte, Ax, Ai, Bx, Bi);
            Ax += dxA;
            Ai += diA;
            Bx += dxB;
            Bi += diB;
            y++;
        }
    }

    /* bottom half: new edge-A slopes from the middle vertex
     * (gamma.gas:859-878); edge accumulators continue */
    dxA = vlm_fx_sdiv((x2 - x1) << 16, (uint32_t)rows_bot);
    diA = vlm_fx_sdiv(i2 - i1, (uint32_t)rows_bot);

    if (skip_top && y1 < 0) {
        /* middle also above: bulk-advance into view
         * (gamma.gas:880-1004) */
        int32_t skip = (-y1) << 16;
        rows_bot = rows_bot + y1 + 1;
        Ax += vlm_fx_fmult(dxA, skip);
        Ai += vlm_fx_fmult(diA, skip);
        Bx += vlm_fx_fmult(dxB, skip);
        Bi += vlm_fx_fmult(diB, skip);
    }
    if (y1 >= GAMMA_YMAX)
        return;
    if (y2 >= GAMMA_YMAX) {
        rows_bot -= y2 - GAMMA_YMAX;
        if (rows_bot <= 0)
            return;
    }
    for (int32_t r = 0; r < rows_bot; r++) {
        gamma_span(c, y, colbyte, Ax, Ai, Bx, Bi);
        Ax += dxA;
        Ai += diA;
        Bx += dxB;
        Bi += diB;
        y++;
    }
}

/* ---- vertex emit through the reduced symmetry path ------------------- */

/* Rotational-copy emitter (gamma.gas:350-459): like the shared rot
 * engine but without step-delta, colour phases, or mirror bits; each
 * copy emits one vertex. Non-rot emits the vertex once. Returns the
 * emit count (identical for all three vertices of a face). */
static int gamma_emit(VlmFxCtx *c, int32_t x, int32_t y, uint16_t code,
                      GEmit *out)
{
    const uint8_t *fx = c->fx;
    if (!(c->asym & 0x10000)) {
        out[0].x = x;
        out[0].y = y;
        out[0].code = code;
        return 1;
    }
    int32_t count = (int32_t)vlm_be32(fx + FX_RSYM_ORD);
    uint32_t step = vlm_be32(fx + FX_RSYM_STE);
    uint32_t accum = vlm_be32(fx + FX_RSYM_PHASE);
    const uint8_t *tab = vlm_wave_table_from_addr(vlm_be32(fx + FX_SINE_BAS));
    uint32_t rx = vlm_be32(fx + FX_RXCEN);
    uint32_t ry = vlm_be32(fx + FX_RYCEN);
    int32_t dx0 = x - ((int32_t)rx >> 16);
    int32_t dy0 = y - ((int32_t)ry >> 16);
    int32_t dx = dx0, dy = dy0;
    int n = 0;

    for (;;) {
        accum += step; /* no step-delta accumulation in gamma */
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

        if (n < MAX_COPIES) {
            out[n].x = px + ((int32_t)rx >> 16);
            out[n].y = py + ((int32_t)ry >> 16);
            out[n].code = code;
            n++;
        }

        dx = vlm_fx_imult(vlm_be32(fx + FX_ROSCAL2) + 0x1000,
                          (uint32_t)dx0) >> 12;
        dy = vlm_fx_imult(vlm_be32(fx + FX_ROSCALEI) + 0x1000,
                          (uint32_t)dy0) >> 12;
        rx += vlm_be32(fx + FX_DRXCEN);
        ry += vlm_be32(fx + FX_DRYCEN);
        if (--count < 0)
            break;
    }
    return n;
}

void vlm_fx_gamma(VlmFxCtx *c)
{
    uint8_t *fx = c->fx;

    /* beat gate (gamma.gas:6-18) */
    uint32_t trig = vlm_be32(fx + FX_HEIGHT);
    uint32_t mtrig = vlm_be32(fx + FX_MTRIG);
    if (mtrig && (trig & mtrig) != mtrig)
        return;

    c->asym = vlm_be32(fx + FX_ASYM_FLA);

    /* byte-resolution rotation basis (gamma.gas:19-41) */
    const uint8_t *tab = vlm_wave_table_from_addr(vlm_be32(fx + FX_SINE_BAS));
    uint32_t idx = (vlm_be32(fx + FX_PHASE1) >> 16) & 0xFF;
    int32_t s = (int32_t)tab[idx] - 128;
    int32_t co = (int32_t)tab[(idx + 64) & 0xFF] - 128;

    uint32_t sx = vlm_be32(fx + FX_J) - 0x8000;
    uint32_t sy = vlm_be32(fx + FX_K) - 0x8000;

    const uint8_t *obj = vlm_polyos[(vlm_be32(fx + FX_THANG) >> 16) & 7];
    if (!obj)
        return; /* polyos[0] is the vlmlogo2 CRY image, not a mesh */

    uint32_t nfaces = vlm_be16(obj);
    int32_t cx = (int32_t)vlm_be16(obj + 2);
    int32_t cy = (int32_t)vlm_be16(obj + 4);
    const uint8_t *verts = obj + 6 + nfaces * 16;
    int32_t ox = (int32_t)vlm_be32(fx + FX_DSTOFFX) >> 16;
    int32_t oy = (int32_t)vlm_be32(fx + FX_DSTOFFY) >> 16;

    static GEmit emit[3][MAX_COPIES];

    for (uint32_t f = 0; f < nfaces; f++) {
        const uint8_t *rec = obj + 6 + f * 16;
        uint32_t colw = (uint32_t)vlm_be16(rec) << 8;
        int copies = 0;

        for (int v = 0; v < 3; v++) {
            uint32_t vidx = vlm_be16(rec + 2 + v * 4);
            uint16_t code = vlm_be16(rec + 4 + v * 4);
            int32_t vx = (int32_t)vlm_be16(verts + vidx * 4);
            int32_t vy = (int32_t)vlm_be16(verts + vidx * 4 + 2);

            /* deform + rotate (gamma.gas:111-141): logical >>4 then
             * lo16 imult, exactly as the hardware sequence */
            uint32_t tx = (uint32_t)(vx - cx) >> 4;
            uint32_t ty = (uint32_t)(vy - cy) >> 4;
            int32_t dvx = vlm_fx_imult(sx, tx) >> 11;
            int32_t dvy = vlm_fx_imult(sy, ty) >> 11;
            int32_t X = (vlm_fx_imult((uint32_t)co, (uint32_t)dvx)
                         - vlm_fx_imult((uint32_t)s, (uint32_t)dvy)) >> 7;
            int32_t Y = (vlm_fx_imult((uint32_t)co, (uint32_t)dvy)
                         + vlm_fx_imult((uint32_t)s, (uint32_t)dvx)) >> 7;
            copies = gamma_emit(c, X + ox, Y + oy,
                                (uint16_t)(colw | code), emit[v]);
        }

        for (int k = 0; k < copies; k++) {
            /* corner intensities live from _i1.._i6 (gamma.gas:294-306);
             * <<8 into the blitter's intensity format */
            int32_t ia = (int32_t)(vlm_be32(fx + 0x54
                             + (emit[0][k].code & 0x1C)) << 8);
            int32_t ib = (int32_t)(vlm_be32(fx + 0x54
                             + (emit[1][k].code & 0x1C)) << 8);
            int32_t ic = (int32_t)(vlm_be32(fx + 0x54
                             + (emit[2][k].code & 0x1C)) << 8);
            gamma_triangle(c, emit[0][k].code >> 8,
                           emit[0][k].x, emit[0][k].y, ia,
                           emit[1][k].x, emit[1][k].y, ib,
                           emit[2][k].x, emit[2][k].y, ic);
        }
    }
}
