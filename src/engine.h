/*
 * VLM engine core: bank/effect state, oscillator bank, wave-link
 * modulation matrix, ADSR envelopes, spectrum band envelopes.
 *
 * Mirrors the runtime split of the original:
 *  - 68k side: ADSR (doadsr, vlm.s:906), effect select (skidoo,
 *    vlm.s:4595), bank load (gm, via banks.c)
 *  - GPU omega.gas: per-fxobj oscillator update + BigLoop64 modulation
 *  - GPU psi.gas: spectrum peak-hold, band envelopes, triggers
 *
 * All fxobj state lives in the big-endian matrix image, exactly as on
 * the Jaguar; fields are accessed via vlm_be32/vlm_wbe32.
 */
#ifndef VLM_ENGINE_H
#define VLM_ENGINE_H

#include <stdint.h>

#include "banks.h"

typedef struct {
    uint8_t matrix[VLM_MATRIX_SIZE]; /* decompressed current bank */
    uint8_t *fx[VLM_SUBFX];          /* live fxobj slots (NULL if empty) */
    int bank;                        /* imatrix, 0..9 */
    int effect;                      /* skid, 1..9 */

    /* pixcon..delayn globals (108-byte image, restored by skidoo) */
    uint8_t pixcon[108];

    /* Animation state */
    uint16_t envvals[8];   /* [0..4] band envelopes, [5]=C [6]=B [7]=A */
    uint16_t oscvals[8];   /* current fxobj's oscillator outputs */
    uint16_t results[64];  /* modulation scalars of the last fxobj pass */

    /* psi.gas audio state */
    uint32_t absdelta[64]; /* peak-hold spectrum */
    uint8_t peakflag[64];
    uint16_t trig;         /* low byte trigger bits, high byte toggles */
    uint8_t trigbits_held;
    uint16_t band_trigmin[5];  /* auto-gain thresholds (from avbank) */

    /* ADSR machines (A, B, C) */
    struct {
        int state;         /* 0 trigwait 1 attack 2 decay 3 sustain 4 release */
        int32_t level;     /* 0..0xFFFF */
    } adsr[3];

    /* User controls (px/py pseudo-waveforms; symadj inertia later) */
    uint16_t px, py;

    /* OP display scale word, written by beta from `thang` */
    uint16_t skale;

    uint32_t buttons;      /* PAD_* bitmask for ADSR triggers */
} VlmEngine;

void vlm_engine_init(VlmEngine *e);

/* Load bank (0..9) and select effect (1..9): gm + skidoo. */
void vlm_engine_select(VlmEngine *e, int bank, int effect);

/* Feed one frame of FFT magnitudes (64+3 longs as the DSP produced
 * at $F1B034); pass NULL for silence. */
void vlm_engine_spectrum(VlmEngine *e, const uint32_t *fft67);

/* Run one 60Hz frame: per fxobj, the drawing module renders into
 * `screen` (384x384 CRY16, may be NULL to skip drawing) with
 * `cscreen` as the previous completed frame (feedback source), then
 * the oscillator + modulation pass animates parameters. */
void vlm_engine_frame(VlmEngine *e, uint16_t *screen,
                      const uint16_t *cscreen);

#endif
