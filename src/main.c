/*
 * VLM-1 SDL2/GLES2 port — entry point.
 *
 * Drawing modules are not ported yet, so this currently runs the full
 * parameter-animation engine (bank/effect load, oscillators, wave-link
 * modulation, ADSR + band envelopes) on a synthetic test spectrum and
 * renders a diagnostic scope into the CRY framebuffer:
 *   top:    64-band absdelta spectrum (peak-hold)
 *   middle: 8 oscillator outputs + 8 envelope values as bars
 *   bottom: the 64 modulation results as a strip chart
 * Keys: 1-9 select effect within bank, * (8) + digit selects bank,
 * A/B/C = Z/X/C trigger the ADSR envelopes.
 */
#include "platform.h"
#include "engine.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define FB_W 384
#define FB_H 240

static uint16_t fb[FB_W * FB_H];
static VlmEngine engine;

/* Synthetic FFT: a slow bass thump + two wandering mid/high peaks, so
 * the psi band envelopes and triggers have something to chew on. */
static void test_spectrum(uint32_t t, uint32_t out[67])
{
    for (int i = 0; i < 67; i++)
        out[i] = 0x00000400;
    if ((t % 45) < 6) /* beat every 0.75s */
        for (int i = 1; i <= 6; i++)
            out[i] = 0xB000;
    int mid = 11 + (int)((sinf((float)t * 0.020f) + 1.0f) * 8.0f);
    int high = 41 + (int)((sinf((float)t * 0.033f) + 1.0f) * 11.0f);
    out[mid] = 0x7000;
    out[mid + 1] = 0x5000;
    out[high] = 0x6000;
    return;
}

static void bar(int x, int y0, int h_up, uint16_t color)
{
    if (x < 0 || x >= FB_W)
        return;
    for (int y = y0; y > y0 - h_up && y >= 0; y--)
        if (y < FB_H)
            fb[y * FB_W + x] = color;
}

static void draw_scope(uint32_t t)
{
    for (int i = 0; i < FB_W * FB_H; i++)
        fb[i] = 0;

    /* absdelta spectrum, 64 bands x 5px wide */
    for (int b = 0; b < 64; b++) {
        int h = (int)(engine.absdelta[b] >> 10);
        if (h > 70)
            h = 70;
        uint16_t c = (uint16_t)(((b * 4) << 8) | 0xE0);
        for (int k = 0; k < 5; k++)
            bar(b * 6 + k + 2, 78, h, c);
    }

    /* oscillator outputs (green-ish) and envelopes (red-ish) */
    for (int k = 0; k < 8; k++) {
        int ho = engine.oscvals[k] >> 10;   /* 0..64 */
        int he = engine.envvals[k] >> 10;
        for (int w = 0; w < 12; w++) {
            bar(20 + k * 20 + w, 158, ho, 0x18F0);
            bar(210 + k * 20 + w, 158, he, 0xC8F0);
        }
    }

    /* modulation results strip */
    for (int i = 0; i < 64; i++) {
        int h = engine.results[i] >> 11;    /* 0..32 */
        for (int k = 0; k < 4; k++)
            bar(i * 6 + k + 2, 232, h + 1, 0x00FF);
    }

    /* trigger lamps */
    for (int b = 0; b < 5; b++) {
        uint16_t c = (engine.trig & (1 << b)) ? 0x38FF : 0x3830;
        for (int w = 0; w < 8; w++)
            bar(340 + b * 9 - 170, 170, 6, c);
        (void)t;
    }
}

int main(int argc, char **argv)
{
    plat_bootstrap(argc, argv);

    PlatformConfig cfg = {
        .fb_width = FB_W,
        .fb_height = FB_H,
        .title = "VLM-1 (SDL2/GLES2)",
        .window_scale = 2,
    };
    if (!plat_init(&cfg))
        return 1;

    vlm_engine_init(&engine);
    vlm_engine_select(&engine, 0, 1);

    uint32_t frame = 0;
    uint32_t held_digits = 0;
    while (plat_poll()) {
        uint32_t buttons = plat_buttons();
        engine.buttons = buttons;

        /* effect/bank select: plain digit = effect, with * held = bank */
        for (int d = 1; d <= 9; d++) {
            uint32_t bit = PAD_DIGIT(d);
            if ((buttons & bit) && !(held_digits & bit)) {
                if (buttons & PAD_STAR)
                    vlm_engine_select(&engine, d - 1, engine.effect);
                else
                    vlm_engine_select(&engine, engine.bank, d);
            }
        }
        held_digits = buttons & (0x3FF * PAD_0);

        uint32_t fft[67];
        test_spectrum(frame, fft);
        vlm_engine_spectrum(&engine, fft);
        vlm_engine_frame(&engine);

        draw_scope(frame);
        plat_present_cry(fb);
        frame++;
    }

    plat_shutdown();
    return 0;
}
