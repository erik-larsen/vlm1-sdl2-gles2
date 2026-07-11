/*
 * Audio spectrum for the engine: converts captured samples into the
 * 64+3 magnitude longs the DSP FFT produced at $F1B034 (ians.s), in
 * the numeric range psi.gas expects (absdelta decay $3000/frame,
 * band trigger thresholds $3000..$7000).
 */
#ifndef VLM_AUDIOFFT_H
#define VLM_AUDIOFFT_H

#include <stdint.h>

#define VLM_FFT_SIZE 256
#define VLM_SPECTRUM_BINS 67

/* samples: VLM_FFT_SIZE mono floats in -1..1; out: 67 magnitude longs. */
void vlm_audio_spectrum(const float *samples,
                        uint32_t out[VLM_SPECTRUM_BINS]);

#endif
