#ifndef VOCODER_BATCH_BIQUAD_H
#define VOCODER_BATCH_BIQUAD_H

// Batch biquad filter abstraction using CMSIS-DSP arm_biquad_cascade_df2T_f32.
//
// On ARM target the library uses hand-optimised assembly with pipeline-aware
// scheduling for the Cortex-M7 FPU.
//
// On host (macOS/Linux tests) the same CMSIS-DSP source is compiled but the
// generic C fallback is selected automatically (no ARM SIMD extensions defined).
// This means the tests exercise the real CMSIS-DSP code paths, including the
// pCoeffs/pState pointer lifetime semantics that the host-only reimplementation
// previously masked.

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

inline float batchBiquadProcessWithEnvelope(const BatchBiquadCoeffs &,
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

// Convert DF1 feedback coefficients to CMSIS-DSP DF2T format.
//
// The descriptor stores a1/a2 with the DF1 sign convention:
//   y[n] = b0*x[n] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]
// so a stable bandpass has a1 < 0 and a2 > 0.
//
// CMSIS-DSP DF2T uses the opposite sign in its state update:
//   d1 += a1_cmsis * y[n]   (positive, not negative)
// so a1_cmsis = -a1_df1 and a2_cmsis = -a2_df1.
inline BatchBiquadCoeffs batchBiquadFromDF1(float b0, float b2, float a1,
                                            float a2) {
  BatchBiquadCoeffs c;
  c.coeffs[0] = b0;
  c.coeffs[1] = 0.0f; // b1 = 0 for bandpass
  c.coeffs[2] = b2;
  c.coeffs[3] = -a1; // negate: DF1 sign convention is opposite to CMSIS-DSP
  c.coeffs[4] = -a2;
  return c;
}

// Update pCoeffs pointer after the BatchBiquadCoeffs struct is reassigned.
// CMSIS-DSP stores a raw pointer to the coefficients array; call this whenever
// the target BatchBiquadCoeffs object is updated in place so the instance keeps
// pointing at valid memory.
inline void batchBiquadUpdateCoeffs(BatchBiquadState &s,
                                    const BatchBiquadCoeffs &c) {
  s.inst.pCoeffs = c.coeffs;
}

// Reseat pCoeffs and pState pointers without zeroing state.
// Use instead of batchBiquadInit when the filter is already running and you
// only need to point the instance at (possibly-moved) memory.
// batchBiquadInit zeros the state via CMSIS init, causing audible zipper noise
// on live filters (e.g. bandwidth sweeps).
inline void batchBiquadReseat(BatchBiquadState &s, const BatchBiquadCoeffs &c) {
  s.inst.pCoeffs = c.coeffs;
  s.inst.pState  = s.state;
}

// Process one DF2T biquad stage (b1=0 bandpass) and immediately apply
// per-sample smoothed gain, accumulating into accum[].
// Fuses the synthesis filter pass with the gain-smoothing+accumulation pass
// into a single N-sample loop, eliminating the intermediate synthesisBuf.
// gainMix and gainMixComp (= 1 - gainMix) must be pre-computed for the block.
inline void batchBiquadProcessAndAccum(BatchBiquadState &s,
                                       const float *src, float *accum,
                                       int blockSize, float &gainState,
                                       float gainTarget, float gainMix,
                                       float gainMixComp,
                                       float bandGainScale) {
  float d1  = s.state[0];
  float d2  = s.state[1];
  const float b0  = s.inst.pCoeffs[0];
  const float b2  = s.inst.pCoeffs[2];
  const float a1c = s.inst.pCoeffs[3]; // stored as -a1_df1 (CMSIS DF2T sign)
  const float a2c = s.inst.pCoeffs[4]; // stored as -a2_df1
  for (int i = 0; i < blockSize; ++i) {
    const float x = src[i];
    const float y = b0 * x + d1;
    d1 = d2 + a1c * y;          // b1 = 0 for bandpass; CMSIS: d1 = b1*x + a1*y + d2
    d2 = b2 * x + a2c * y;      // CMSIS: d2 = b2*x + a2*y
    gainState = gainMix * gainState + gainMixComp * gainTarget;
    accum[i] += y * gainState * bandGainScale;
  }
  s.state[0] = d1;
  s.state[1] = d2;
}

#endif // VOCODER_BATCH_BIQUAD_H
