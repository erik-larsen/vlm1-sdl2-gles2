/*
 * beta module: Digital Video Feedback (DVF) — the signature feedback
 * zoom/rotate. Port of gpu/beta.gas (line references throughout).
 *
 * The original is one blitter pass: A1 = source = cscreen (previous
 * completed frame) stepped per dest pixel by a 16.16 rotation vector,
 * A2 = dest = the draw screen window, command SRCSHADE|LFU_S|GOURZ|
 * DSTA2|UPDA2|UPDA1|UPDA1F|CLIP_A1|SRCEN ($41802F41, beta.gas:157).
 * SRCSHADE adds the sign-extended top byte of the 24-bit B_IINC value
 * to each source pixel's CRY intensity with saturation (Virtual
 * Jaguar blit.c semantics; GOURZ is set only because SRCSHADE needs
 * it on Jaguar I silicon).
 *
 * Every reachable run first clears the whole screen to the folded
 * base colour at zero intensity (beta.gas:119-128; the border-fill
 * parameter blocks at 129-151 are dead code — set up, never called).
 * The window-degenerate bail path also clears (beta.gas:516-525).
 *
 * Source reads outside the previous frame are treated as write-
 * inhibited (dest keeps the cleared colour). On hardware CLIP_A1 is
 * enabled with A1_CLIP=0 (vlmgpu.s:45) and A1 is the read pointer, so
 * exact edge behavior is murky; this matches the visible black
 * surround. TODO: verify against a BigPEmu capture at extreme zoom.
 */
#include "fx.h"
#include "banks.h"
#include "waves.h"

/* Unsigned interp minus TOPNEN ($8000): the $8000-centred tables give
 * a signed -32768..32767 result (beta.gas:236-274). */
static int32_t wave_centered(const uint8_t *tab, uint32_t idx,
                             uint32_t frac)
{
    int32_t s0 = tab[idx], s1 = tab[idx + 1];
    return ((s0 << 8) + (s1 - s0) * (int32_t)frac) - 0x8000;
}

static void clear_screen(VlmFxCtx *c, uint16_t pix)
{
    for (int i = 0; i < VLM_SCREEN_W * VLM_SCREEN_H; i++)
        c->screen[i] = pix;
}

void vlm_fx_beta(VlmFxCtx *c)
{
    uint8_t *fx = c->fx;

    /* Base colour: narrow fold, intensity 0 (beta.gas:7-34). */
    uint16_t fill = (uint16_t)vlm_fx_fold(vlm_be32(fx + FX_COLX),
                                          vlm_be32(fx + FX_COLY));

    int32_t w = (int32_t)(vlm_be32(fx + FX_DVF_WS_X) >> 16);
    int32_t h = (int32_t)(vlm_be32(fx + FX_DVF_WS_Y) >> 16);

    /* thang>>16 -> the OP display scale word (beta.gas:46-54). */
    c->skale = (uint16_t)(vlm_be32(fx + FX_THANG) >> 16);

    /* Window placement and clamping (beta.gas:55-112). The margins
     * use a LOGICAL shift of (384-w), as the original does. */
    int32_t left = (int32_t)(((uint32_t)(384 - w)) >> 1)
                   + ((int32_t)vlm_be32(fx + FX_VFB_XPOS) >> 16) - 192;
    int32_t top = (int32_t)(((uint32_t)(384 - h)) >> 1)
                  + ((int32_t)vlm_be32(fx + FX_VFB_YPOS) >> 16) - 192;

    int valid = 1;
    if (left < 0) {
        w += left;
        left = 0;
    } else if (left >= 384) {
        valid = 0;
    }
    if (valid) {
        if (top < 0) {
            h += top;
            top = 0;
        } else if (top >= 384) {
            valid = 0;
        }
    }
    if (valid && (w <= 0 || h <= 0 || w >= 384 || h >= 384))
        valid = 0;

    /* Both the bail path and the normal path clear the full screen
     * (beta.gas:119-128, 516-525). */
    clear_screen(c, fill);
    if (!valid)
        return;

    /* Intensity delta: 24-bit (deltai - $800000); SRCSHADE adds the
     * sign-extended top byte (beta.gas:160-185). */
    uint32_t delta24 = (vlm_be32(fx + FX_DVF_DELTAI) - 0x800000) & 0xFFFFFF;
    int32_t delta = (int32_t)((delta24 >> 16) & 0xFF);
    if (delta & 0x80)
        delta -= 0x100;

    const uint16_t *src = c->cscreen;
    uint16_t *dst = c->screen;

    if (vlm_be32(fx + FX_ITER2) != 0) {
        /* Straight-copy variant (beta.gas:530-563): same window,
         * source shifted vertically by (dvf_crot_y - $C00000)>>16. */
        int32_t dy = (int16_t)((vlm_be32(fx + FX_DVF_CROT_Y)
                                - 0xC00000) >> 16);
        for (int32_t row = 0; row < h; row++) {
            int32_t sy = top + row + dy;
            for (int32_t col = 0; col < w; col++) {
                int32_t sx = left + col;
                if (sx < 0 || sx >= 384 || sy < 0 || sy >= 384)
                    continue;
                uint16_t s = src[sy * VLM_SCREEN_W + sx];
                int32_t y = (s & 0xFF) + delta;
                if (y < 0)
                    y = 0;
                if (y > 0xFF)
                    y = 0xFF;
                dst[(top + row) * VLM_SCREEN_W + left + col] =
                    (uint16_t)((s & 0xFF00) | y);
            }
        }
        return;
    }

    /* Rotate/zoom path. Vectors (beta.gas:225-388): */
    const uint8_t *tab = vlm_wave_table_from_addr(vlm_be32(fx + FX_SINE_BAS));
    uint32_t angl = vlm_be32(fx + FX_VFB_ANGL);
    uint32_t idx = (angl >> 16) & 0xFF;
    uint32_t frac = (angl >> 8) & 0xFF;
    int32_t sinv = wave_centered(tab, idx, frac);
    /* cosine: index (idx+1+63)&$FF with the same fraction
     * (beta.gas:257-274) */
    int32_t cosv = wave_centered(tab, (idx + 64) & 0xFF, frac);

    int32_t sx16 = (int32_t)(vlm_fx_mult((uint32_t)w,
                       vlm_be32(fx + FX_VFB_XSCA) >> 16) << 8);
    int32_t sy16 = (int32_t)(vlm_fx_mult((uint32_t)h,
                       vlm_be32(fx + FX_VFB_YSCA) >> 16) << 8);

    int32_t Xx = vlm_fx_fmult(sx16, cosv);
    int32_t Xy = vlm_fx_fmult(sx16, sinv);
    int32_t Yx = -vlm_fx_fmult(sy16, sinv);
    int32_t Yy = vlm_fx_fmult(sy16, cosv);

    /* Per-pixel increment and per-row step; the step subtracts the
     * RAW row vector Xx/Xy, not w*xinc (beta.gas:390-448), so the
     * truncation residue of the divide accumulates exactly as on
     * hardware. */
    int32_t xincX = vlm_fx_sdiv(Xx, (uint32_t)w);
    int32_t xincY = vlm_fx_sdiv(Xy, (uint32_t)w);
    int32_t stepX = vlm_fx_sdiv(Yx, (uint32_t)h) - Xx;
    int32_t stepY = vlm_fx_sdiv(Yy, (uint32_t)h) - Xy;

    /* Source start corner around the centre of rotation
     * (beta.gas:449-465). */
    int32_t srcX = (int32_t)vlm_be32(fx + FX_DVF_CROT_X) + 0x8000
                   - (Xx >> 1) - (Yx >> 1);
    int32_t srcY = (int32_t)vlm_be32(fx + FX_DVF_CROT_Y) + 0x8000
                   - (Xy >> 1) - (Yy >> 1);

    for (int32_t row = 0; row < h; row++) {
        int32_t ax = srcX, ay = srcY;
        uint16_t *drow = dst + (top + row) * VLM_SCREEN_W + left;
        for (int32_t col = 0; col < w; col++) {
            /* hardware A1 x/y are 16-bit signed */
            int32_t px = (int16_t)(ax >> 16);
            int32_t py = (int16_t)(ay >> 16);
            if (px >= 0 && px < 384 && py >= 0 && py < 384) {
                uint16_t s = src[py * VLM_SCREEN_W + px];
                int32_t y = (s & 0xFF) + delta;
                if (y < 0)
                    y = 0;
                if (y > 0xFF)
                    y = 0xFF;
                drow[col] = (uint16_t)((s & 0xFF00) | y);
            }
            ax += xincX;
            ay += xincY;
        }
        srcX = ax + stepX;
        srcY = ay + stepY;
    }
}
