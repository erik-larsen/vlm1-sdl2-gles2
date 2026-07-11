/*
 * shu module: "spectrum as intensities" — 64 dots whose brightness
 * follows the absdelta spectrum bands. Port of gpu/shu.gas (line
 * references throughout).
 *
 * Differences from the alpha-family shared code, all faithful:
 * - post-symmetry translation (crot_x/y>>16 - $C0) added by the plot
 *   wrapper AFTER the mirror chain (shu.gas:36-48, 468-477);
 * - the mirror chain reloads the last PLOTTED point at each step
 *   (transforms don't chain through unplotted steps; shu.gas:399-467);
 * - plot table (verified against vlm.abs, module image at 0x193d0):
 *   {1x1, 2x2, size-dispatch, size-dispatch}, with the blitter size
 *   taken from ysca = (k>>8)|1, not pixsize (shu.gas:125-130, 520-529).
 */
#include "fx.h"
#include "banks.h"
#include "waves.h"

static int32_t shu_tx, shu_ty;   /* (r14+$1e/$1f) */
static uint32_t shu_ysca;        /* alt r18 */

/* Plot wrapper (routine425): translate then dispatch on the module's
 * own table. */
static void shu_plot(VlmFxCtx *c, int32_t x, int32_t y)
{
    x += shu_tx;
    y += shu_ty;
    switch (c->plot_mode & 3) {
    case 0:
        vlm_fx_plot_1x1(c, x, y);
        break;
    case 1:
        vlm_fx_plot_2x2(c, x, y);
        break;
    default:
        /* size dispatch on ysca: 0 -> 1x1, 1 -> 2x2, else blitter
         * n x n (n >= 2 goes to the bounds-checked fill) */
        if (shu_ysca == 0)
            vlm_fx_plot_1x1(c, x, y);
        else if (shu_ysca == 1)
            vlm_fx_plot_2x2(c, x, y);
        else
            vlm_fx_plot_fill_n(c, x, y, (int32_t)shu_ysca);
        break;
    }
}

/* shu's mirror chain (shu.gas:399-467): each step transforms the
 * saved (last-plotted) point; plotting updates the saved point. */
static void shu_chain(VlmFxCtx *c, int32_t x, int32_t y)
{
    const int32_t W = VLM_CLIP_W;
    int32_t sx = x, sy = y;   /* (r14+$12/$13) */
    int32_t cx = x, cy = y;
    uint32_t bits = c->asym;
    for (;;) {
        if (bits & 1) {
            sx = cx;
            sy = cy;
            shu_plot(c, cx, cy);
        }
        cx = W - sx;
        cy = sy;
        if (bits & 2) {
            sx = cx;
            sy = cy;
            shu_plot(c, cx, cy);
        }
        cx = sx;
        cy = W - sy;
        if (bits & 4) {
            sx = cx;
            sy = cy;
            shu_plot(c, cx, cy);
        }
        cx = W - sx;
        cy = sy;
        if (bits & 8) {
            sx = cx;
            sy = cy;
            shu_plot(c, cx, cy);
        }
        /* step D (routine407): diagonal swap of the saved point */
        cx = sy;
        cy = W - sx;
        bits >>= 4;
        if (bits & 0x10)
            break;
        bits |= 0x100;
    }
}

static uint32_t sat16(uint32_t v)
{
    return v > 0xFFFF ? 0xFFFF : v;
}

void vlm_fx_shu(VlmFxCtx *c)
{
    uint8_t *fx = c->fx;

    c->asym = vlm_be32(fx + FX_ASYM_FLA);
    c->plot_mode = (int)vlm_be32(fx + FX_PLOT_MOD);
    c->pixscale = vlm_be32(fx + FX_PIXSIZE) >> 16;
    c->plot = shu_plot;
    c->chain = shu_chain;

    shu_tx = ((int32_t)vlm_be32(fx + FX_DVF_CROT_X) >> 16) - 0xC0;
    shu_ty = ((int32_t)vlm_be32(fx + FX_DVF_CROT_Y) >> 16) - 0xC0;
    shu_ysca = (vlm_be32(fx + FX_K) >> 8) | 1;

    uint32_t colx = vlm_be32(fx + FX_COLX);
    uint32_t coly = vlm_be32(fx + FX_COLY);
    int32_t cvx4 = (int32_t)vlm_be32(fx + FX_CVX2) >> 4;
    int32_t cvy4 = (int32_t)vlm_be32(fx + FX_CVY2) >> 4;

    int32_t pix = (int32_t)c->pixscale;
    int32_t bx = ((int32_t)vlm_be32(fx + FX_DSTOFFX) >> 16) - 32 * pix;
    int32_t by = ((int32_t)vlm_be32(fx + FX_DSTOFFY) >> 16) - (pix >> 1);

    /* Bands 63..0 read absdelta[0..63]: the spectrum runs right to
     * left (shu.gas:140-201). fxobj phase3 ($78) gets transient
     * per-band stores but is restored to its original value on exit
     * (shu.gas:202-205) — net unchanged, so we don't touch it. */
    for (int32_t band = 63; band >= 0; band--) {
        uint32_t v = sat16(c->absdelta[63 - band] << 2);
        int32_t y = by - (int32_t)((v << 1) >> 11);

        c->cc_pix = vlm_fx_fold(colx, coly);
        /* crossed on purpose: coly advances by cvx2>>4, colx by
         * cvy2>>4 (shu.gas:181-188) */
        coly += (uint32_t)cvx4;
        colx += (uint32_t)cvy4;

        c->intensity = sat16(v << 3);
        int32_t x = (int32_t)vlm_fx_mult((uint32_t)band,
                                         (uint32_t)pix) + bx;
        vlm_fx_symmetry_plot(c, x, y);
    }
}
