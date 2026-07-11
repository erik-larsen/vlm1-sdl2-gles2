#include "waves.h"
#include "waves_data.h"
#include "banks.h" /* VLM_P_SINES_ADDR */

uint8_t vlm_p_sines[257];
uint8_t vlm_p_saw[257];
uint8_t vlm_p_square[257];
uint8_t vlm_p_ramp[257];
uint8_t vlm_p_rect[257];

/* 257-entry view of rantab for the noise waveform. */
static const uint8_t *rantab_table;

void vlm_waves_init(void)
{
    /* p_sines / p_rect from the signed sine bytes; the original first
     * copies sines[0] to sines[256] and loops 257 times
     * (vlm.s:1956-1971). */
    for (int n = 0; n <= 256; n++) {
        int8_t s = (int8_t)vlm_sines_raw[n & 0xFF];
        int v = s < 0 ? -s : s;
        vlm_p_rect[n] = (uint8_t)(v << 1);
        vlm_p_sines[n] = (uint8_t)(s + 0x80);
    }

    /* p_saw (triangle) and p_ramp (vlm.s:1973-1987). */
    for (int n = 0; n < 256; n++) {
        int d1 = n * 2 - 0xFF;
        if (d1 < 0)
            d1 = -d1;
        vlm_p_saw[n] = (uint8_t)d1;
        vlm_p_ramp[n] = (uint8_t)n;
    }
    vlm_p_saw[256] = vlm_p_saw[0];   /* move.b p_saw,(p_square-4).l */
    vlm_p_ramp[256] = 0;             /* never written by the original */

    /* p_square: $FF for the first half, then 0 (vlm.s:1991-2001). */
    for (int n = 0; n < 256; n++)
        vlm_p_square[n] = n < 128 ? 0xFF : 0x00;
    vlm_p_square[256] = vlm_p_square[0];

    rantab_table = vlm_rantab; /* 516 bytes, plenty for [idx+1] */
}

const uint8_t *vlm_wave_table(int type)
{
    switch (type) {
    case WAVE_SAW:    return vlm_p_saw;
    case WAVE_SQUARE: return vlm_p_square;
    case WAVE_RAMP:   return vlm_p_ramp;
    case WAVE_RECT:   return vlm_p_rect;
    case WAVE_NOISE:  return rantab_table;
    default:          return vlm_p_sines;
    }
}

const uint8_t *vlm_wave_table_from_addr(uint32_t jag_addr)
{
    /* sine_bas/wave_2 hold p_sines + 0x104*n (tables are 260 bytes
     * apart); anything unrecognized falls back to sine. */
    uint32_t rel = jag_addr - VLM_P_SINES_ADDR;
    if (rel % 0x104 == 0 && rel / 0x104 <= 4) {
        static uint8_t *const tables[5] = {
            vlm_p_sines, vlm_p_saw, vlm_p_square, vlm_p_ramp, vlm_p_rect,
        };
        return tables[rel / 0x104];
    }
    return vlm_p_sines;
}
