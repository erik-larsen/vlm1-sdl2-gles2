/*
 * VLM-1 SDL2/GLES2 port — entry point.
 *
 * For now this runs a CRY test pattern that exercises the full CRY color
 * space, to validate the platform layer and CRY->RGB conversion on both
 * native and Emscripten builds. The VLM engine replaces vlm_frame() as
 * the port progresses.
 */
#include "platform.h"

#include <stdint.h>
#include <stdlib.h>

/* Jaguar VLM effect framebuffer dimensions (placeholder until confirmed
 * from the reverse-engineered source; the engine will own this). */
#define FB_W 384
#define FB_H 240

static uint16_t fb[FB_W * FB_H];

/* Test pattern: left half sweeps the 16x16 chroma grid with intensity
 * animated; right half draws intensity ramps and a moving bar. */
static void vlm_frame(uint32_t t)
{
    for (int y = 0; y < FB_H; y++) {
        for (int x = 0; x < FB_W / 2; x++) {
            int cc = ((y * 16 / FB_H) << 4) | (x * 16 / (FB_W / 2));
            int iy = (x * 2 + y + t) & 0xFF;
            fb[y * FB_W + x] = (uint16_t)((cc << 8) | iy);
        }
        for (int x = FB_W / 2; x < FB_W; x++) {
            int cc = (y * 256 / FB_H) & 0xFF;
            int iy = ((x - FB_W / 2) * 255 / (FB_W / 2 - 1)) & 0xFF;
            fb[y * FB_W + x] = (uint16_t)((cc << 8) | iy);
        }
    }
    /* moving full-intensity bar */
    int bx = (int)(t % FB_W);
    for (int y = 0; y < FB_H; y++)
        fb[y * FB_W + bx] = 0x00FF; /* white (chroma 0 = grey axis) */
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

    uint32_t frame = 0;
    while (plat_poll()) {
        vlm_frame(frame++);
        plat_present_cry(fb);
    }

    plat_shutdown();
    return 0;
}
