/*
 * VLM-1 SDL2/GLES2 port — entry point.
 *
 * Runs the parameter-animation engine over the real preset banks with
 * a synthetic test spectrum, rendering ported drawing modules (alpha
 * so far) into double-buffered 384x384 CRY screens, exactly like the
 * original's dscreen/cscreen pair. Unported modules (notably beta,
 * the DVF feedback that fades/zooms the previous frame) are no-ops,
 * so plots currently accumulate — expected until beta lands.
 *
 * Keys: 1-9 effect, 8(*)+digit bank, Z/X/C = A/B/C buttons (ADSR),
 * V toggles the engine diagnostic scope, Esc quits.
 */
#include "platform.h"
#include "engine.h"
#include "fx.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static uint16_t screens[2][VLM_SCREEN_W * VLM_SCREEN_H];
static int draw_idx;
static VlmEngine engine;
static bool show_scope;
static uint16_t scope_fb[VLM_SCREEN_W * VLM_SCREEN_H];

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
}

static void bar(uint16_t *fb, int x, int y0, int h_up, uint16_t color)
{
    if (x < 0 || x >= VLM_SCREEN_W)
        return;
    for (int y = y0; y > y0 - h_up && y >= 0; y--)
        if (y < VLM_SCREEN_H)
            fb[y * VLM_SCREEN_W + x] = color;
}

static void draw_scope(void)
{
    memset(scope_fb, 0, sizeof scope_fb);
    for (int b = 0; b < 64; b++) {
        int h = (int)(engine.absdelta[b] >> 9);
        if (h > 120)
            h = 120;
        uint16_t c = (uint16_t)(((b * 4) << 8) | 0xE0);
        for (int k = 0; k < 5; k++)
            bar(scope_fb, b * 6 + k + 2, 130, h, c);
    }
    for (int k = 0; k < 8; k++) {
        for (int w = 0; w < 12; w++) {
            bar(scope_fb, 20 + k * 20 + w, 250, engine.oscvals[k] >> 9,
                0x18F0);
            bar(scope_fb, 210 + k * 20 + w, 250, engine.envvals[k] >> 9,
                0xC8F0);
        }
    }
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 4; k++)
            bar(scope_fb, i * 6 + k + 2, 370, (engine.results[i] >> 10) + 1,
                0x00FF);
    for (int b = 0; b < 5; b++) {
        uint16_t c = (engine.trig & (1 << b)) ? 0x38FF : 0x3830;
        for (int w = 0; w < 8; w++)
            bar(scope_fb, 170 + b * 12 + w, 262, 6, c);
    }
}

static bool tick(void)
{
    static uint32_t frame = 0;
    static uint32_t held_digits = 0;
    static bool held_v = false;

    if (!plat_poll())
        return false;

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

    if ((buttons & PAD_SCOPE) && !held_v)
        show_scope = !show_scope;
    held_v = (buttons & PAD_SCOPE) != 0;

    uint32_t fft[67];
    test_spectrum(frame, fft);
    vlm_engine_spectrum(&engine, fft);
    vlm_engine_frame(&engine, screens[draw_idx]);

    if (show_scope) {
        draw_scope();
        plat_present_cry(scope_fb);
    } else {
        plat_present_cry(screens[draw_idx]);
    }
    draw_idx ^= 1; /* swap dscreen/cscreen */
    frame++;
    return true;
}

int main(int argc, char **argv)
{
    plat_bootstrap(argc, argv);

    PlatformConfig cfg = {
        .fb_width = VLM_SCREEN_W,
        .fb_height = VLM_SCREEN_H,
        .title = "VLM-1 (SDL2/GLES2)",
        .window_scale = 2,
    };
    if (!plat_init(&cfg))
        return 1;

    vlm_engine_init(&engine);
    vlm_engine_select(&engine, 0, 1);

    plat_run(tick);

    plat_shutdown();
    return 0;
}
