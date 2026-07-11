#include "audiofft.h"

#include <math.h>

#define N VLM_FFT_SIZE

/* Iterative in-place radix-2 complex FFT. */
static void fft(float *re, float *im)
{
    /* bit-reverse permutation */
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j |= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * (float)M_PI / (float)len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < N; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

void vlm_audio_spectrum(const float *samples,
                        uint32_t out[VLM_SPECTRUM_BINS])
{
    static float hann[N];
    static int hann_ready;
    if (!hann_ready) {
        for (int i = 0; i < N; i++)
            hann[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i
                                          / (float)(N - 1)));
        hann_ready = 1;
    }

    float re[N], im[N];
    for (int i = 0; i < N; i++) {
        re[i] = samples[i] * hann[i];
        im[i] = 0.0f;
    }
    fft(re, im);

    /* Magnitude scaling: a full-scale sine lands its band peak around
     * $8000, matching the trigger thresholds' working range (Hann
     * coherent gain 0.5 folded into the factor). */
    for (int b = 0; b < VLM_SPECTRUM_BINS; b++) {
        float m = sqrtf(re[b] * re[b] + im[b] * im[b]) * 512.0f;
        if (m > 1.0e9f)
            m = 1.0e9f;
        out[b] = (uint32_t)m;
    }
}
