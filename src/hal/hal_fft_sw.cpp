/**
 * hal_fft_sw.cpp
 * Software Cooley-Tukey radix-2 DIT FFT -- no platform library required.
 *
 * Sufficient for 128-point FFT at 0.5 Hz trigger rate (~127 trig calls per
 * transform, negligible on both LX7 and M4F at sub-Hz trigger rates).
 *
 * Replace with an optimised platform implementation (esp-dsp / CMSIS-DSP) by
 * swapping this file. The halFft() signature in hal_fft.h stays the same.
 */
#ifdef METER_AFE

#include "hal/hal_fft.h"
#include <math.h>

static void bitReverse(float* buf, int32_t n) {
  for (int32_t i = 1, j = 0; i < n; i++) {
    int32_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float tr = buf[2 * i];     float ti = buf[2 * i + 1];
      buf[2 * i]     = buf[2 * j];     buf[2 * i + 1] = buf[2 * j + 1];
      buf[2 * j]     = tr;             buf[2 * j + 1] = ti;
    }
  }
}

void halFft(float* buf, int32_t n) {
  bitReverse(buf, n);
  for (int32_t len = 2; len <= n; len <<= 1) {
    float ang  = -2.0f * 3.14159265358979f / (float)len;
    float wRe  = cosf(ang);
    float wIm  = sinf(ang);
    for (int32_t i = 0; i < n; i += len) {
      float curRe = 1.0f, curIm = 0.0f;
      for (int32_t j = 0; j < len / 2; j++) {
        int32_t u  = i + j;
        int32_t v  = u + len / 2;
        float uRe  = buf[2 * u];
        float uIm  = buf[2 * u + 1];
        float vRe  = buf[2 * v] * curRe - buf[2 * v + 1] * curIm;
        float vIm  = buf[2 * v] * curIm + buf[2 * v + 1] * curRe;
        buf[2 * u]     = uRe + vRe;  buf[2 * u + 1] = uIm + vIm;
        buf[2 * v]     = uRe - vRe;  buf[2 * v + 1] = uIm - vIm;
        float nextRe   = curRe * wRe - curIm * wIm;
        curIm          = curRe * wIm + curIm * wRe;
        curRe          = nextRe;
      }
    }
  }
}

#endif // METER_AFE
