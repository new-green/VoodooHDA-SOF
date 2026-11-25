/*
 * rnnoise_kern.h — interface of the kernel build of RNNoise v0.1.1 (see README.voodoo).
 * Single static instance: callers must serialize every call.
 */
#ifndef RNNOISE_KERN_H
#define RNNOISE_KERN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RNNK_FRAME	480	/* samples per call, 48 kHz mono, 16-bit scale (+-32768) */

/* (re)initialize the state; gain_floor = lowest band gain (0 = stock RNNoise). 0 on success */
int rnnk_init(float gain_floor);
/* lowest band gain for the following frames */
void rnnk_set_floor(float gain_floor);
/* denoise one frame (output is delayed by one frame); returns the voice probability */
float rnnk_process(float *out, const float *in);

#ifdef __cplusplus
}
#endif

#endif
