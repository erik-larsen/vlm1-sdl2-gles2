/*
 * Effect (GPU module) infrastructure: the drawing context shared by
 * all ported .gas modules, plus the shared plot / symmetry / colour
 * helpers (see docs/analysis-gpu-effects.md §3-4).
 *
 * Screen: 384x384 16bpp CRY, row stride 384 pixels ($300 bytes).
 * Coordinate clip range is 0..$17D against W=$17E, as in the original
 * plot routines.
 */
#ifndef VLM_FX_H
#define VLM_FX_H

#include <stdint.h>

#define VLM_SCREEN_W 384
#define VLM_SCREEN_H 384
#define VLM_CLIP_W 0x17E /* r15 in the modules */

/* Particle pools shared by theta (simulate) and epsilon (render).
 * 32-byte records on the Jaguar (theta.gas comments); native layout
 * here since only these two modules touch them. */
#define VLM_PARTICLE_SLOTS 512

typedef struct {
    int32_t x, y, z;        /* 16.16 */
    uint16_t colour;        /* CC<<8 pixel word */
    uint16_t life;          /* intensity; 0 = free slot */
    int32_t vx, vy, vz;
} VlmParticle;

typedef struct {
    int32_t count;
    VlmParticle p[VLM_PARTICLE_SLOTS];
} VlmParticlePool;

typedef struct VlmFxCtx VlmFxCtx;
struct VlmFxCtx {
    uint16_t *screen;     /* draw buffer (r23 / dscreen) */
    const uint16_t *cscreen; /* previous completed frame (feedback src) */
    uint8_t *fx;          /* fxobj big-endian byte image (r3) */
    uint32_t submode;     /* low word of the gpu field ($F03F5C) */
    int echo_run;         /* draw_screen+4 bit0: 1 = delay-line echo run */
    uint16_t skale;       /* OP display-scale word written by beta */
    const uint32_t *absdelta; /* 64-band peak-hold spectrum (lol[7]) */
    VlmParticlePool *pools;   /* pobjs[2] (lol[3]) */

    /* Module-register state used by the shared plot path */
    uint32_t cc_pix;      /* r5: CC << 8 */
    uint32_t intensity;   /* r27: 16-bit intensity */
    uint32_t asym;        /* r22: asym_fla */
    uint32_t pixscale;    /* (r14+$1b): pixsize >> 16 */
    int plot_mode;        /* plot_mod field ($E8), 0..3 */

    /* Per-module hooks: final plot dispatch (the module's plot-routine
     * table) and the mirror-chain variant. NULL selects the alpha
     * versions. */
    void (*plot)(VlmFxCtx *c, int32_t x, int32_t y);
    void (*chain)(VlmFxCtx *c, int32_t x, int32_t y);
};

/* Shared helpers (fx_common.c) */

/* Narrow CRY colour fold of two 16.16 phases -> CC<<8 (§4.1). */
uint32_t vlm_fx_fold(uint32_t colx, uint32_t coly);

/* Signed interpolated waveform lookup: ((t[i]-128)<<8)+(t[i+1]-t[i])*frac
 * with i=(phase>>16)&255, frac=(phase>>8)&255 (§4.2). */
int32_t vlm_fx_wave_signed(const uint8_t *tab, uint32_t phase);

/* Unsigned variant (no -128 bias). */
int32_t vlm_fx_wave_unsigned(const uint8_t *tab, uint32_t phase);

/* Full symmetry entry ($F03392): rotational-symmetry engine when
 * asym_fla bit16 is set, else the mirror chain, ending in plots. */
void vlm_fx_symmetry_plot(VlmFxCtx *c, int32_t x, int32_t y);

/* Building-block plots shared by module plot tables. */
void vlm_fx_plot_1x1(VlmFxCtx *c, int32_t x, int32_t y);
void vlm_fx_plot_2x2(VlmFxCtx *c, int32_t x, int32_t y);
/* Bounds-checked n x n fill centred on (x,y) (the blitter path); does
 * NOT redirect small n to the fast paths. */
void vlm_fx_plot_fill_n(VlmFxCtx *c, int32_t x, int32_t y, int32_t n);

/* GPU unsigned divide (div by 0 yields all-ones like the hardware). */
static inline uint32_t vlm_fx_udiv(uint32_t n, uint32_t d)
{
    return d ? n / d : 0xFFFFFFFFu;
}

/* The signed-divide idiom (§4.4): abs, unsigned div, sign restore. */
static inline int32_t vlm_fx_sdiv(int32_t n, uint32_t d)
{
    uint32_t un = n < 0 ? 0u - (uint32_t)n : (uint32_t)n;
    uint32_t q = vlm_fx_udiv(un, d);
    return n < 0 ? -(int32_t)q : (int32_t)q;
}

/* imult: signed 16x16 -> 32 (low 16 bits of each operand). */
static inline int32_t vlm_fx_imult(uint32_t a, uint32_t b)
{
    return (int32_t)(int16_t)a * (int16_t)b;
}

/* mult: unsigned 16x16 -> 32. */
static inline uint32_t vlm_fx_mult(uint32_t a, uint32_t b)
{
    return (uint32_t)(uint16_t)a * (uint16_t)b;
}

/* fmult: sign-magnitude 16.16 multiply (omega.gas:71). */
static inline int32_t vlm_fx_fmult(int32_t a, int32_t b)
{
    int neg = (a < 0) ^ (b < 0);
    uint32_t ua = a < 0 ? 0u - (uint32_t)a : (uint32_t)a;
    uint32_t ub = b < 0 ? 0u - (uint32_t)b : (uint32_t)b;
    uint32_t r = (uint32_t)(((uint64_t)ua * ub) >> 16);
    return neg ? -(int32_t)r : (int32_t)r;
}

/* Modules */
void vlm_fx_alpha(VlmFxCtx *c);
void vlm_fx_beta(VlmFxCtx *c);
void vlm_fx_shu(VlmFxCtx *c);
void vlm_fx_delta(VlmFxCtx *c);
void vlm_fx_epsilon(VlmFxCtx *c);
void vlm_fx_theta(VlmFxCtx *c);
void vlm_fx_gamma(VlmFxCtx *c);

#endif
