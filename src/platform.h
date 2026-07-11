/*
 * Platform layer: SDL2 window + GLES2 context + CRY framebuffer present.
 * One shared code path for native (ANGLE on mac/win) and Emscripten
 * (ASYNCIFY lets the app keep its own main loop).
 */
#ifndef VLM_PLATFORM_H
#define VLM_PLATFORM_H

#include <stdint.h>
#include <stdbool.h>

/* Jaguar pad buttons the VLM uses (bitmask). */
enum {
    PAD_UP     = 1 << 0,
    PAD_DOWN   = 1 << 1,
    PAD_LEFT   = 1 << 2,
    PAD_RIGHT  = 1 << 3,
    PAD_A      = 1 << 4,
    PAD_B      = 1 << 5,
    PAD_C      = 1 << 6,
    PAD_OPTION = 1 << 7,
    PAD_PAUSE  = 1 << 8,
    PAD_STAR   = 1 << 9,   /* '*' */
    PAD_HASH   = 1 << 10,  /* '#' */
    PAD_0      = 1 << 11,  /* keypad digits 0-9: bits 11..20 */
};
#define PAD_DIGIT(n) (PAD_0 << (n))

typedef struct {
    int fb_width, fb_height;   /* CRY framebuffer size in pixels */
    const char *title;
    int window_scale;          /* initial window = fb size * scale */
} PlatformConfig;

/* Call first thing in main(). On macOS this may re-exec the process so
 * dyld can find the bundled ANGLE libraries; a no-op elsewhere. */
void plat_bootstrap(int argc, char **argv);

bool plat_init(const PlatformConfig *cfg);
void plat_shutdown(void);

/* Run the main loop: calls tick() once per frame until tick returns
 * false (or the browser tab closes). Native: a plain loop; Emscripten:
 * emscripten_set_main_loop, so no ASYNCIFY is needed. Does not return
 * on Emscripten. */
void plat_run(bool (*tick)(void));

/* Poll input; returns false when the app should quit. Call once per
 * tick. */
bool plat_poll(void);

/* Currently-held pad buttons. */
uint32_t plat_buttons(void);

/* Present a CRY16 framebuffer (fb_width*fb_height uint16 pixels),
 * then wait for the next 60Hz tick. */
void plat_present_cry(const uint16_t *cry_pixels);

/* Milliseconds since init. */
uint32_t plat_ticks(void);

#endif
