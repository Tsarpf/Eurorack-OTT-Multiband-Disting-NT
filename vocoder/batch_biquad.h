#ifndef VOCODER_BATCH_BIQUAD_H
#define VOCODER_BATCH_BIQUAD_H

// Batch biquad filter abstraction layer.
//
// On ARM target: uses CMSIS-DSP arm_biquad_cascade_df2T_f32 (hand-optimized
// assembly with 16x loop unrolling and pipeline-aware instruction scheduling).
//
// On host (macOS/Linux tests): uses a portable C implementation of the same
// DF2T algorithm.

#if defined(__arm__) && !defined(__APPLE__) && !defined(__aarch64__)
// ── ARM target: use actual CMSIS-DSP library ──
#include "dsp/filtering_functions.h"

struct BatchBiquadCoeffs {
  // CMSIS-DSP coefficient order: {b0, b1, b2, a1, a2} per stage
  // For our bandpass: b1 = 0, so coefficients are {b0, 0, b2, a1, a2}
  float coeffs[5];
};

struct BatchBiquadState {
  arm_biquad_cascade_df2T_instance_f32 inst;
  float state[2]; // DF2T: 2 state variables per stage
};

inline void batchBiquadInit(BatchBiquadState &s, const BatchBiquadCoeffs &c) {
  arm_biquad_cascade_df2T_init_f32(&s.inst, 1, c.coeffs, s.state);
}

inline void batchBiquadProcess(const BatchBiquadCoeffs &, BatchBiquadState &s,
                               const float *src, float *dst, int blockSize) {
  arm_biquad_cascade_df2T_f32(&s.inst, src, dst, (uint32_t)blockSize);
}

// CMSIS-DSP doesn't have a built-in envelope variant, so we process then scan
inline float batchBiquadProcessWithEnvelope(const BatchBiquadCoeffs &c,
                                            BatchBiquadState &s,
                                            const float *src, float *dst,
                                            int blockSize) {
  arm_biquad_cascade_df2T_f32(&s.inst, src, dst, (uint32_t)blockSize);
  float peak = 0.0f;
  for (int i = 0; i < blockSize; ++i) {
    const float ay = dst[i] < 0.0f ? -dst[i] : dst[i];
    if (ay > peak)
      peak = ay;
  }
  return peak;
}

inline BatchBiquadCoeffs batchBiquadFromDF1(float b0, float b2, float a1,
                                            float a2) {
  BatchBiquadCoeffs c;
  c.coeffs[0] = b0;
  c.coeffs[1] = 0.0f; // b1 = 0 for bandpass
  c.coeffs[2] = b2;
  c.coeffs[3] = a1;
  c.coeffs[4] = a2;
  return c;
}

// Update coefficients in-place (after smoothing)
inline void batchBiquadUpdateCoeffs(BatchBiquadState &s,
                                    const BatchBiquadCoeffs &c) {
  s.inst.pCoeffs = c.coeffs;
}

#else
// ── Host fallback: portable C implementation of DF2T ──
// Same algorithm as CMSIS-DSP but without ARM-specific optimizations.

struct BatchBiquadCoeffs {
  float b0;
  float b1; // always 0 for bandpass, kept for API compatibility
  float b2;
  float a1;
  float a2;
};

struct BatchBiquadState {
  float d1;
  float d2;
};

inline void batchBiquadInit(BatchBiquadState &s, const BatchBiquadCoeffs &) {
  s.d1 = 0.0f;
  s.d2 = 0.0f;
}

inline void batchBiquadProcess(const BatchBiquadCoeffs &c, BatchBiquadState &s,
                               const float *__restrict src,
                               float *__restrict dst, int blockSize) {
  float d1 = s.d1;
  float d2 = s.d2;
  for (int i = 0; i < blockSize; ++i) {
    const float x = src[i];
    const float y = c.b0 * x + d1;
    d1 = c.b1 * x - c.a1 * y + d2;
    d2 = c.b2 * x - c.a2 * y;
    dst[i] = y;
  }
  s.d1 = d1;
  s.d2 = d2;
}

inline float batchBiquadProcessWithEnvelope(const BatchBiquadCoeffs &c,
                                            BatchBiquadState &s,
                                            const float *__restrict src,
                                            float *__restrict dst,
                                            int blockSize) {
  float d1 = s.d1;
  float d2 = s.d2;
  float peak = 0.0f;
  for (int i = 0; i < blockSize; ++i) {
    const float x = src[i];
    const float y = c.b0 * x + d1;
    d1 = c.b1 * x - c.a1 * y + d2;
    d2 = c.b2 * x - c.a2 * y;
    dst[i] = y;
    const float ay = y < 0.0f ? -y : y;
    if (ay > peak)
      peak = ay;
  }
  s.d1 = d1;
  s.d2 = d2;
  return peak;
}

inline BatchBiquadCoeffs batchBiquadFromDF1(float b0, float b2, float a1,
                                            float a2) {
  BatchBiquadCoeffs c;
  c.b0 = b0;
  c.b1 = 0.0f;
  c.b2 = b2;
  c.a1 = a1;
  c.a2 = a2;
  return c;
}

inline void batchBiquadUpdateCoeffs(BatchBiquadState &,
                                    const BatchBiquadCoeffs &) {
  // No-op on host — coefficients are read directly from BatchBiquadCoeffs
}

#endif // __arm__

#endif // VOCODER_BATCH_BIQUAD_H
