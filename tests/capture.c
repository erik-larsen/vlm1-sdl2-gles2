/*
 * Deterministic, GL-free capture harness for fidelity work.
 *
 * Runs the real engine + effect modules against the synthetic (hence
 * reproducible) test spectrum, then writes the OP-visible 384x240 view
 * as a binary PPM. Same per-frame order as main.c's tick so the output
 * matches what the app renders. No board/logo overlay (pure effect).
 *
 *   capture <bank 0-9> <effect 1-9> <frames> <out.ppm>
 */
#include "../src/engine.h"
#include "../src/fx.h"
#include "../src/cry.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define VIEW_W 384
#define VIEW_H 240

static uint16_t screens[2][VLM_SCREEN_W * VLM_SCREEN_H];
static uint16_t view[VIEW_W * VIEW_H];
static VlmEngine engine;

/* byte-identical to main.c:test_spectrum */
static void test_spectrum(uint32_t t, uint32_t out[67])
{
    for (int i = 0; i < 67; i++)
        out[i] = 0x00000400;
    if ((t % 45) < 6)
        for (int i = 1; i <= 6; i++)
            out[i] = 0xB000;
    int mid = 11 + (int)((sinf((float)t * 0.020f) + 1.0f) * 8.0f);
    int high = 41 + (int)((sinf((float)t * 0.033f) + 1.0f) * 11.0f);
    out[mid] = 0x7000;
    out[mid + 1] = 0x5000;
    out[high] = 0x6000;
}

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

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <bank 0-9> <effect 1-9> <frames> <out.ppm>\n",
                argv[0]);
        return 2;
    }
    int bank = atoi(argv[1]), effect = atoi(argv[2]), frames = atoi(argv[3]);
    const char *out = argv[4];

    cry_init();
    vlm_engine_init(&engine);
    vlm_engine_select(&engine, bank, effect);

    int draw = 0;
    for (uint32_t f = 0; f < (uint32_t)frames; f++) {
        vlm_engine_frame(&engine, screens[draw], screens[draw ^ 1]);
        uint32_t fft[67];
        test_spectrum(f, fft);
        vlm_engine_spectrum(&engine, fft);
        build_view(screens[draw]);
        draw ^= 1;
    }

    FILE *fp = fopen(out, "wb");
    if (!fp) {
        perror("fopen");
        return 1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", VIEW_W, VIEW_H);
    for (int i = 0; i < VIEW_W * VIEW_H; i++) {
        uint32_t p = cry_to_rgba[view[i]]; /* 0xAABBGGRR */
        uint8_t rgb[3] = { p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF };
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    fprintf(stderr, "wrote %s (bank %d effect %d, %d frames, skale %d)\n",
            out, bank, effect, frames, engine.skale);
    return 0;
}
