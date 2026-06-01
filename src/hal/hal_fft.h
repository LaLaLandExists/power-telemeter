/**
 * hal_fft.h
 * Platform-agnostic radix-2 FFT on a complex interleaved buffer.
 *
 * halFft() computes an in-place forward DFT. buf is interleaved
 * [Re0, Im0, Re1, Im1, ...] of length n*2 floats. n must be a power of 2;
 * the AFE pipeline uses n=128.
 *
 * Current implementation:
 *   hal/hal_fft_sw.cpp -- portable Cooley-Tukey radix-2 DIT (no library dep)
 *
 * To substitute a platform-optimised library (esp-dsp dsps_fft2r_fc32,
 * CMSIS-DSP arm_cfft_f32): replace hal_fft_sw.cpp with a platform-specific
 * file that implements the same signature. All call sites remain unchanged.
 *
 * Only compiled when METER_AFE is defined (guarded inside implementation).
 */
#pragma once
#include <stdint.h>

void halFft(float* buf, int32_t n);
