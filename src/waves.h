/*
 * Oscillator waveform tables, expanded at init exactly like `startup`
 * (vlm.s:1955-2001). All tables have 257 entries because the
 * oscillator output lerps between table[idx] and table[idx+1].
 */
#ifndef VLM_WAVES_H
#define VLM_WAVES_H

#include <stdint.h>

/* Waveform type codes (oscbank entry +14; wts/sineslookup,
 * vlm.s:7666-7685). */
enum {
    WAVE_SINE = 0,
    WAVE_SAW = 1,       /* triangle |2i-255| in the original naming */
    WAVE_SQUARE = 2,
    WAVE_RAMP = 3,      /* no table: output = phase>>8 */
    WAVE_RECT = 4,      /* rectified sine */
    WAVE_NOISE = 5,     /* rantab */
    WAVE_CONST = 6,     /* delta forced 0 */
    WAVE_USER_Y = 7,    /* py */
    WAVE_USER_X = 8,    /* px */
};

extern uint8_t vlm_p_sines[257];
extern uint8_t vlm_p_saw[257];
extern uint8_t vlm_p_square[257];
extern uint8_t vlm_p_ramp[257];
extern uint8_t vlm_p_rect[257];

/* Map a waveform type (0..5) to its table; types 3 and 6..8 are
 * handled specially by the oscillator code. Returns rantab-backed
 * table for WAVE_NOISE. */
const uint8_t *vlm_wave_table(int type);

/* Map a Jaguar address stored in sine_bas/wave_2 to a table. */
const uint8_t *vlm_wave_table_from_addr(uint32_t jag_addr);

void vlm_waves_init(void);

#endif
