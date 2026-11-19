/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Sampling profiler of the GPU thread (PS5C_GPU_SAMPLE=<seconds>, gpu_sampler.c). */
#ifndef PS5C_GPU_SAMPLER_H
#define PS5C_GPU_SAMPLER_H

/* Called on the GPU thread: samples it from a thread of its own when PS5C_GPU_SAMPLE is set. */
void gpu_sampler_start(void);

#endif
