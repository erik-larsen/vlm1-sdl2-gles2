#include "banks.h"
#include "banks_data.h"

#include <string.h>

/* Six pristine fx objects — the delta-compression base layer
 * (refblock, vlm.s:292-297). */
static uint8_t refblock[VLM_EFFECT_SIZE];
static int refblock_ready;

/* oscbank: 8 records of {value, increment, waveform ptr, index}
 * (vlm.s:8423-8430). */
static const uint32_t oscbank[8][4] = {
    { 0x0400000, 0x0011100, VLM_P_SINES_ADDR, 0 },
    { 0x0000000, 0x0016300, VLM_P_SINES_ADDR, 0 },
    { 0x0000000, 0x0003000, VLM_P_SINES_ADDR, 0 },
    { 0x0000000, 0x000C000, VLM_P_SINES_ADDR, 0 },
    { 0x0000000, 0x0010000, VLM_P_SINES_ADDR, 0 },
    { 0x0000000, 0x000E000, VLM_P_SINES_ADDR, 0 },
    { 0x0000000, 0x000C300, VLM_P_SINES_ADDR, 0 },
    { 0x0000000, 0x000F310, VLM_P_SINES_ADDR, 0 },
};

/* The pixcon..delayn global block, initial values (vlm.s:8432-8451):
 * pixcon/piycon 7 longs each, adsra/b (10 bytes), adsrc (12 bytes),
 * py, px, delayf, delayp, dline, delayt, delayn -- 108 bytes. */
static const uint8_t pixcon_init[108] = {
    /* pixcon */
    0x00,0x80,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x16,0x00,
    0x00,0x00,0x30,0x00, 0x00,0xFF,0xFF,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x04,0x00,0x00,
    /* piycon */
    0x00,0x80,0x00,0x00, 0x00,0x00,0x00,0x00, 0x00,0x00,0x16,0x00,
    0x00,0x00,0x30,0x00, 0x00,0xFF,0xFF,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x04,0x00,0x00,
    /* adsra */ 0x0A,0,1,0, 0xC0,0,5,0, 0,4,
    /* adsrb */ 0x0B,0,1,0, 0xC0,0,5,0, 0,4,
    /* adsrc */ 0x0C,0,2,0, 0xC0,0,5,0, 0,4,0,0,
    /* py, px */ 0,0,0,0, 0,0,0,0,
    /* delayf, delayp */ 0,0, 0,0,
    /* dline */ 0,0,0,0,
    /* delayt, delayn */ 0,0, 0,0,
};

void vlm_ifxobj(uint8_t *obj)
{
    memset(obj, 0, VLM_FXOBJ_SIZE);

    /* Default parameters, in the exact order of ifxobj
     * (vlm.s:1816-1871). */
    vlm_wbe32(obj + FX_COL1, 0x00008000);
    vlm_wbe32(obj + FX_DSTOFFX, 0x00C00000);
    vlm_wbe32(obj + FX_DSTOFFY, 0x00C00000);
    vlm_wbe32(obj + FX_DSTOFFZ, 0);
    vlm_wbe32(obj + FX_PHASE1, 0x00000000);
    vlm_wbe32(obj + FX_PHASE2, 0x00000400);
    vlm_wbe32(obj + FX_ITER, 0x00140000);
    vlm_wbe32(obj + FX_J, 0x00000400);
    vlm_wbe32(obj + FX_K, 0x00000400);
    vlm_wbe32(obj + FX_ZAMP, 0x00000400);
    vlm_wbe32(obj + FX_ASYM_FLA, 3);
    vlm_wbe32(obj + FX_SINE_BAS, VLM_P_SINES_ADDR);
    vlm_wbe32(obj + FX_DY, 0x00200000);
    vlm_wbe32(obj + FX_RSYM_ORD, 8);
    vlm_wbe32(obj + FX_RSYM_STE, 0x00080000);
    vlm_wbe32(obj + FX_RSYM_IST, 0x00000000);
    vlm_wbe32(obj + FX_RXCEN, 0x00C00000);
    vlm_wbe32(obj + FX_RYCEN, 0x00C00000);
    vlm_wbe32(obj + FX_ROSCALE, 0x00000800);
    vlm_wbe32(obj + FX_ROSCAL2, 0x00000000);
    vlm_wbe32(obj + FX_ROSCALEI, 0x00000000);
    vlm_wbe32(obj + FX_DRXCEN, 0x00000000);
    vlm_wbe32(obj + FX_DRYCEN, 0x00000000);
    vlm_wbe32(obj + FX_CVX, 0x00001000);
    vlm_wbe32(obj + FX_CVY, 0x00020000);
    vlm_wbe32(obj + FX_RADIUS, 0x00080000);
    vlm_wbe32(obj + FX_I1, 0x00008000);
    vlm_wbe32(obj + FX_I2, 0x00008000);
    vlm_wbe32(obj + FX_I3, 0x00008000);
    vlm_wbe32(obj + FX_I4, 0x00008000);
    vlm_wbe32(obj + FX_I5, 0x00008000);
    vlm_wbe32(obj + FX_I6, 0x00008000);
    vlm_wbe32(obj + FX_WAVE_2, VLM_P_SINES_ADDR);
    vlm_wbe32(obj + FX_PHASE4, 0x00000000);
    vlm_wbe32(obj + FX_PHASE5, 0x00021555);
    vlm_wbe32(obj + FX_PLOT_MOD, 0x00000000);
    vlm_wbe32(obj + FX_VFB_XSCA, 0x01850000);
    vlm_wbe32(obj + FX_VFB_YSCA, 0x01850000);
    vlm_wbe32(obj + FX_VFB_ANGL, 0x00000000);
    vlm_wbe32(obj + FX_DVF_CROT_X, 0x00C00000);
    vlm_wbe32(obj + FX_DVF_CROT_Y, 0x00C00000);
    vlm_wbe32(obj + FX_DVF_DELTAI, 0x00780000);
    vlm_wbe32(obj + FX_DVF_WS_X, 0x00000000);
    vlm_wbe32(obj + FX_DVF_WS_Y, 0x00E00000);
    vlm_wbe32(obj + FX_VFB_XPOS, 0x00C00000);
    vlm_wbe32(obj + FX_VFB_YPOS, 0x00C00000);
    vlm_wbe32(obj + FX_COLX, 0x0008F000);
    vlm_wbe32(obj + FX_COLY, 0x0008F000);
    vlm_wbe32(obj + FX_CVX2, 0x00010000);
    vlm_wbe32(obj + FX_CVY2, 0x00010000);
    vlm_wbe32(obj + FX_PIXSIZE, 0x00000000);
    vlm_wbe32(obj + FX_PHASE4, 0x00400000); /* set twice in the original */
    vlm_wbe32(obj + FX_THANG, 0);
    vlm_wbe32(obj + FX_INFO, 0x00000000);
    vlm_wbe32(obj + FX_HEIGHT, 0);
    vlm_wbe32(obj + FX_MTRIG, 0);

    /* Bytes 256-511: longs of $0000FFFF; bytes 512-767 stay zero. */
    for (int n = 256; n < 512; n += 4)
        vlm_wbe32(obj + n, 0x0000FFFF);

    /* Bytes 768-895: oscbank image. */
    for (int r = 0; r < 8; r++)
        for (int w = 0; w < 4; w++)
            vlm_wbe32(obj + 768 + r * 16 + w * 4, oscbank[r][w]);

    /* Bytes 896-1003: pixcon..delayn image. */
    memcpy(obj + 896, pixcon_init, sizeof pixcon_init);
}

void vlm_init_refblock(void)
{
    for (int s = 0; s < VLM_SUBFX; s++)
        vlm_ifxobj(refblock + s * VLM_FXOBJ_SIZE);
    refblock_ready = 1;
}

/* Patch-decompress one effect in place (loop `unp`, vlm.s:7054-7063).
 * Returns the stream pointer after the terminating pair; the caller
 * rewinds by 2 for the next effect (vlm.s:7083). */
static const uint8_t *unpack_effect(const uint8_t *src, uint8_t *dst)
{
    uint32_t pos = 0;
    for (;;) {
        uint8_t skip = *src++;
        uint8_t value = *src++;
        if (pos >= VLM_EFFECT_SIZE)
            break;
        pos += skip;
        if (pos >= VLM_EFFECT_SIZE)
            break;
        dst[pos++] = value;
    }
    return src;
}

void vlm_load_bank(int bank, uint8_t *matrix)
{
    if (!refblock_ready)
        vlm_init_refblock();

    /* gm fills the whole matrix with $FF first (vlm.s:7020-7025). */
    memset(matrix, 0xFF, VLM_MATRIX_SIZE);

    /* Source: av1-relative offset from the bank table (vlm.s:7027). */
    const uint8_t *src = vlm_av1 + vlm_bank_off[bank];

    /* Effect 1 starts from refblock; each further effect starts from a
     * copy of the previous one, and the terminating pair of each
     * stream is re-consumed by the next (vlm.s:7052-7097). */
    memcpy(matrix, refblock, VLM_EFFECT_SIZE);
    src = unpack_effect(src, matrix);
    for (int e = 1; e < VLM_NEFFECTS; e++) {
        uint8_t *dst = matrix + e * VLM_EFFECT_SIZE;
        memcpy(dst, dst - VLM_EFFECT_SIZE, VLM_EFFECT_SIZE);
        src = unpack_effect(src - 2, dst);
    }
}
