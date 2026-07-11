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
 * Keys: 1-9 effect, M(*)+digit bank, Z/X/C = A/B/C buttons (ADSR),
 * A toggles live audio capture vs the synthetic test signal,
 * V toggles the engine diagnostic scope, Esc quits.
 */
#include "platform.h"
#include "engine.h"
#include "fx.h"
#include "audiofft.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* NTSC visible window of the 384x384 buffer, as the Object Processor
 * presented it (beastie 0: X=-8, Y=-132 half-lines, vlm.s:338-349;
 * clip0/clip2 + MakeScaledObject). skale (0/1/2, driven per frame by
 * beta from the DVF's thang) zooms 1x/2x/4x into a window whose
 * origin drops to keep the kaleidoscope cell centred:
 * source = (384>>skale) x (240>>skale) at (0, 132>>(skale+1)). */
#define VIEW_W 384
#define VIEW_H 240

static uint16_t screens[2][VLM_SCREEN_W * VLM_SCREEN_H];
static uint16_t view[VIEW_W * VIEW_H];
static int draw_idx;
static VlmEngine engine;
static bool show_scope;
static bool use_audio;      /* live capture vs synthetic test signal */
static bool audio_running;
static uint16_t scope_fb[VIEW_W * VIEW_H];

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
    if (x < 0 || x >= VIEW_W)
        return;
    for (int y = y0; y > y0 - h_up && y >= 0; y--)
        if (y < VIEW_H)
            fb[y * VIEW_W + x] = color;
}

static void draw_scope(void)
{
    memset(scope_fb, 0, sizeof scope_fb);
    for (int b = 0; b < 64; b++) {
        int h = (int)(engine.absdelta[b] >> 10);
        if (h > 80)
            h = 80;
        uint16_t c = (uint16_t)(((b * 4) << 8) | 0xE0);
        for (int k = 0; k < 5; k++)
            bar(scope_fb, b * 6 + k + 2, 84, h, c);
    }
    for (int k = 0; k < 8; k++) {
        for (int w = 0; w < 12; w++) {
            bar(scope_fb, 20 + k * 20 + w, 160, engine.oscvals[k] >> 10,
                0x18F0);
            bar(scope_fb, 210 + k * 20 + w, 160, engine.envvals[k] >> 10,
                0xC8F0);
        }
    }
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 4; k++)
            bar(scope_fb, i * 6 + k + 2, 232, (engine.results[i] >> 11) + 1,
                0x00FF);
    for (int b = 0; b < 5; b++) {
        uint16_t c = (engine.trig & (1 << b)) ? 0x38FF : 0x3830;
        for (int w = 0; w < 8; w++)
            bar(scope_fb, 170 + b * 12 + w, 172, 6, c);
    }
}

/* Extract the OP-visible window into the 384x240 view (nearest
 * neighbour, as the OP's integer HSCALE/VSCALE replication did). */
static void build_view(const uint16_t *src)
{
    unsigned s = engine.skale <= 2 ? engine.skale : 0;
    int oy = 132 >> (s + 1);
    for (int y = 0; y < VIEW_H; y++) {
        const uint16_t *srow = src + (oy + (y >> s)) * VLM_SCREEN_W;
        uint16_t *drow = view + y * VIEW_W;
        for (int x = 0; x < VIEW_W; x++)
            drow[x] = srow[x >> s];
    }
}

static bool tick(void)
{
    static uint32_t frame = 0;
    static uint32_t held_digits = 0;
    static bool held_v = false;
    static bool held_a = false;

    if (!plat_poll())
        return false;

    uint32_t buttons = plat_buttons();
    engine.buttons = buttons;

    if ((buttons & PAD_AUDIO) && !held_a) {
        use_audio = !use_audio;
        if (use_audio && !audio_running)
            audio_running = plat_audio_start();
    }
    held_a = (buttons & PAD_AUDIO) != 0;

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

    /* Modules read the psi outputs of the PREVIOUS frame (psi runs
     * after all fxobjs in omega), so render first, then update the
     * spectrum state. */
    vlm_engine_frame(&engine, screens[draw_idx], screens[draw_idx ^ 1]);

    uint32_t fft[VLM_SPECTRUM_BINS];
    float samples[VLM_FFT_SIZE];
    if (use_audio && audio_running
        && plat_audio_latest(samples, VLM_FFT_SIZE))
        vlm_audio_spectrum(samples, fft);
    else
        test_spectrum(frame, fft);
    vlm_engine_spectrum(&engine, fft);

    if (show_scope) {
        draw_scope();
        plat_present_cry(scope_fb);
    } else {
        build_view(screens[draw_idx]);
        plat_present_cry(view);
    }
    draw_idx ^= 1; /* swap dscreen/cscreen */
    frame++;
    return true;
}

int main(int argc, char **argv)
{
    plat_bootstrap(argc, argv);

    PlatformConfig cfg = {
        .fb_width = VIEW_W,
        .fb_height = VIEW_H,
        .title = "VLM-1 (SDL2/GLES2)",
        .window_scale = 2,
    };
    if (!plat_init(&cfg))
        return 1;

    vlm_engine_init(&engine);
    vlm_engine_select(&engine, 0, 1);

    /* Try live audio up front (browser shows its mic permission
     * prompt here); the synthetic test spectrum remains the fallback
     * and the A key switches between them. */
    audio_running = plat_audio_start();
    use_audio = audio_running;

    plat_run(tick);

    plat_shutdown();
    return 0;
}
