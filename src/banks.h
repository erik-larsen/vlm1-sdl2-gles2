/*
 * Bank data loading: builds the default fx-object base layer (ifxobj)
 * and delta-decompresses the 9 effects of a bank into a matrix image,
 * exactly mirroring `gm` in the original 68k code (vlm.s:7017).
 *
 * All buffers are big-endian byte images, as on the Jaguar: the patch
 * stream addresses individual bytes, so fields are only interpreted
 * (via vlm_be32 etc.) after decompression.
 */
#ifndef VLM_BANKS_H
#define VLM_BANKS_H

#include <stdint.h>

#define VLM_FXOBJ_SIZE 1024
#define VLM_SUBFX 6                              /* fx objects per effect */
#define VLM_EFFECT_SIZE (VLM_SUBFX * VLM_FXOBJ_SIZE) /* 6144 */
#define VLM_NEFFECTS 9
#define VLM_MATRIX_SIZE (VLM_NEFFECTS * VLM_EFFECT_SIZE) /* 55296 */

/* Field offsets within an fx object's 256-byte parameter block
 * (vlm.inc:210-294). */
enum {
    FX_DVF_WS_X = 0x04, FX_DVF_WS_Y = 0x08,
    FX_VFB_XSCA = 0x0C, FX_VFB_YSCA = 0x10, FX_VFB_ANGL = 0x14,
    FX_DVF_CROT_X = 0x18, FX_DVF_CROT_Y = 0x1C, FX_DVF_DELTAI = 0x20,
    FX_DSTOFFX = 0x24, FX_DSTOFFY = 0x28,
    FX_VFB_XPOS = 0x2C, FX_VFB_YPOS = 0x30, FX_DSTOFFZ = 0x34,
    FX_VECTORX = 0x38, FX_DY = 0x3C, FX_VECTORY = 0x40,
    FX_SYMTYPES = 0x44, FX_RSYM_ORD = 0x48, FX_RSYM_STE = 0x4C,
    FX_RSYM_IST = 0x50,
    FX_I1 = 0x54, FX_I2 = 0x58, FX_I3 = 0x5C,
    FX_I4 = 0x60, FX_I5 = 0x64, FX_I6 = 0x68,
    FX_ZAMP = 0x6C, FX_PHASE1 = 0x70, FX_PHASE2 = 0x74,
    FX_RSYM_PHASE = 0x78, FX_PHASE4 = 0x7C,
    FX_ITER = 0x80, FX_J = 0x84, FX_K = 0x88, FX_ITER2 = 0x8C,
    FX_COL1 = 0x90, FX_DELTAZ = 0x94, FX_THANG = 0x98,
    FX_ASYM_FLA = 0xA0, FX_SINE_BAS = 0xA4,
    FX_RXCEN = 0xA8, FX_RYCEN = 0xAC,
    FX_ROSCALE = 0xB0, FX_ROSCAL2 = 0xB4,
    FX_CVX = 0xB8, FX_CVY = 0xBC, FX_ROSCALEI = 0xC0,
    FX_DRXCEN = 0xC4, FX_DRYCEN = 0xC8, FX_PHASE5 = 0xCC,
    FX_WAVE_2 = 0xD0, FX_RADIUS = 0xD4,
    FX_CVX2 = 0xD8, FX_CVY2 = 0xDC, FX_COLX = 0xE0, FX_COLY = 0xE4,
    FX_PLOT_MOD = 0xE8, FX_PIXSIZE = 0xEC, FX_HEIGHT = 0xF0,
    FX_MTRIG = 0xF4, FX_INFO = 0xF8, FX_GPU = 0xFC,
};

/* Jaguar address of p_sines; sine_bas/wave_2 hold p_sines + 0x104*n
 * (five 260-byte waveform tables, then rantab via oscillator index). */
#define VLM_P_SINES_ADDR 0x001E7540u

static inline uint32_t vlm_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t vlm_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void vlm_wbe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Build one pristine 1024-byte fx object (ifxobj, vlm.s:1808). */
void vlm_ifxobj(uint8_t *obj);

/* Build the six-object reference block (vlm.s:292-297). */
void vlm_init_refblock(void);

/* Decompress bank 0..9 into matrix[VLM_MATRIX_SIZE] (gm, vlm.s:7017).
 * Bank 9 (the 10th) is the splash-screen bank. */
void vlm_load_bank(int bank, uint8_t *matrix);

#endif
