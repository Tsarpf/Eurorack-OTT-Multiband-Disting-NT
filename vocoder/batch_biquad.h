#ifndef VOCODER_BATCH_BIQUAD_H
#define VOCODER_BATCH_BIQUAD_H

// Two-section bandpass cascade. The production bank uses trapezoidal
// state-variable sections to preserve float precision at low frequencies.
// CMSIS DF2T remains available for coefficient-reference tests and legacy
// coefficient construction. Both paths use the same persistent state storage.

#include "dsp/filtering_functions.h"
#include <stdint.h>
#include <string.h>

// Avoid memcmp: it is not exported by the NT firmware loader.
inline bool batchBiquadSameSamples(const float *a, const float *b, int count) {
  for (int i = 0; i < count; ++i) {
    uint32_t x, y;
    memcpy(&x, a + i, sizeof(x));
    memcpy(&y, b + i, sizeof(y));
    if (x != y) return false;
  }
  return true;
}

// A routed/formant-shifted filter can converge numerically without ever
// becoming bit-identical. Retire only a residual below 1e-8 bus volts/state
// units; retain larger histories so changes cannot truncate audible tails.
inline bool batchBiquadNegligibleDifference(const float *a, const float *b, int count) {
  for (int i = 0; i < count; ++i) {
    const float difference = a[i] - b[i];
    if (!(difference >= -1.0e-8f && difference <= 1.0e-8f)) return false;
  }
  return true;
}

static const int kVocoderFilterStages = 2;

struct BatchBiquadCoeffs {
  // CMSIS-DSP coefficient order: {b0, b1, b2, a1, a2} per stage
  float coeffs[5 * kVocoderFilterStages];
  // TPT realization of the same poles, used by the production filterbank.
  // {a1,a2,a3,bandGain} avoids subtracting nearly equal denominator
  // coefficients at 20 Hz, where float DF2T loses narrow-band accuracy.
  float svf[4 * kVocoderFilterStages];
  bool useSvf;
};

struct BatchBiquadState {
  arm_biquad_cascade_df2T_instance_f32 inst;
  float state[2 * kVocoderFilterStages];
  const BatchBiquadCoeffs *coefficients;
};

inline void batchBiquadInit(BatchBiquadState &s, const BatchBiquadCoeffs &c) {
  arm_biquad_cascade_df2T_init_f32(&s.inst, kVocoderFilterStages, c.coeffs, s.state);
  s.coefficients = &c;
}

inline void batchBiquadProcess(const BatchBiquadCoeffs &c, BatchBiquadState &s,
                               const float *src, float *dst, int blockSize) {
  if (c.useSvf) {
    for (int stage = 0; stage < kVocoderFilterStages; ++stage) {
      const float *p = c.svf + 4 * stage;
      // Snapshot coefficients so buffer stores cannot force reloads per sample.
      const float p0 = p[0], p1 = p[1], p2 = p[2], p3 = p[3];
      float ic1 = s.state[2 * stage], ic2 = s.state[2 * stage + 1];
      for (int i = 0; i < blockSize; ++i) {
        const float v3 = src[i] - ic2;
        const float v1 = p0 * ic1 + p1 * v3;
        const float v2 = ic2 + p1 * ic1 + p2 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        dst[i] = p3 * v1;
      }
      // Far below any audio level; avoids denormal slow paths on hosts without
      // flush-to-zero and gives decaying silence a finite endpoint.
      s.state[2 * stage] = ic1 > -1.0e-20f && ic1 < 1.0e-20f ? 0.0f : ic1;
      s.state[2 * stage + 1] = ic2 > -1.0e-20f && ic2 < 1.0e-20f ? 0.0f : ic2;
      src = dst;
    }
    return;
  }
  arm_biquad_cascade_df2T_f32(&s.inst, src, dst, (uint32_t)blockSize);
}

inline float batchBiquadProcessWithEnvelope(const BatchBiquadCoeffs &c,
                                            BatchBiquadState &s,
                                            const float *src, float *dst,
                                            int blockSize) {
  batchBiquadProcess(c, s, src, dst, blockSize);
  float peak = 0.0f;
  for (int i = 0; i < blockSize; ++i) {
    const float ay = dst[i] < 0.0f ? -dst[i] : dst[i];
    if (ay > peak)
      peak = ay;
  }
  return peak;
}

template <bool retainOutput, bool measurePower = false>
__attribute__((always_inline)) inline float batchBiquadEnvelope(const BatchBiquadCoeffs &c,
                                     BatchBiquadState &s, const float *src,
                                     int blockSize, float *output, float *meanPower = nullptr) {
  float sumPower = 0.0f;
  if (c.useSvf) {
    const float *p = c.svf;
    // Snapshot coefficients so buffer stores cannot force reloads per sample.
    const float p0 = p[0], p1 = p[1], p2 = p[2], p3 = p[3], p4 = p[4], p5 = p[5], p6 = p[6], p7 = p[7];
    float ic10 = s.state[0], ic20 = s.state[1];
    float ic11 = s.state[2], ic21 = s.state[3];
    float peak = 0.0f;
    for (int index = 0; index < blockSize; ++index) {
      const float v30 = src[index] - ic20;
      const float v10 = p0 * ic10 + p1 * v30;
      const float v20 = ic20 + p1 * ic10 + p2 * v30;
      ic10 = 2.0f * v10 - ic10;
      ic20 = 2.0f * v20 - ic20;
      const float firstOutput = p3 * v10;
      const float v31 = firstOutput - ic21;
      const float v11 = p4 * ic11 + p5 * v31;
      const float v21 = ic21 + p5 * ic11 + p6 * v31;
      ic11 = 2.0f * v11 - ic11;
      ic21 = 2.0f * v21 - ic21;
      const float filtered = p7 * v11;
      if (retainOutput) output[index] = filtered;
      if (measurePower) sumPower += filtered * filtered;
      const float magnitude = filtered < 0.0f ? -filtered : filtered;
      if (magnitude > peak) peak = magnitude;
    }
    s.state[0] = ic10 > -1.0e-20f && ic10 < 1.0e-20f ? 0.0f : ic10;
    s.state[1] = ic20 > -1.0e-20f && ic20 < 1.0e-20f ? 0.0f : ic20;
    s.state[2] = ic11 > -1.0e-20f && ic11 < 1.0e-20f ? 0.0f : ic11;
    s.state[3] = ic21 > -1.0e-20f && ic21 < 1.0e-20f ? 0.0f : ic21;
    if (measurePower) *meanPower = sumPower / (float)blockSize;
    return peak;
  }
  float filtered[24];
  float peak = 0.0f;
  for (int offset = 0; offset < blockSize; offset += 24) {
    const int count = blockSize - offset < 24 ? blockSize - offset : 24;
    batchBiquadProcess(c, s, src + offset, filtered, count);
    if (retainOutput)
      for (int i = 0; i < count; ++i) output[offset + i] = filtered[i];
    for (int index = 0; index < count; ++index) {
      const float magnitude =
          filtered[index] < 0.0f ? -filtered[index] : filtered[index];
      if (magnitude > peak) peak = magnitude;
      if (measurePower) sumPower += filtered[index] * filtered[index];
    }
  }
  if (measurePower) *meanPower = sumPower / (float)blockSize;
  return peak;
}

inline float batchBiquadEnvelopeOnly(const BatchBiquadCoeffs &c,
                                     BatchBiquadState &s, const float *src,
                                     int count) {
  return batchBiquadEnvelope<false>(c, s, src, count, nullptr);
}

inline void batchBiquadAccumFiltered(const float *filtered, float *accum,
                                     int count, float &gainState,
                                     float target, float mix, float complement,
                                     float bandGain) {
  for (int i = 0; i < count; ++i) {
    gainState = mix * gainState + complement * target;
    accum[i] += filtered[i] * gainState * bandGain;
  }
  if (gainState >= 0.0f && gainState < 1.0e-20f) gainState = 0.0f;
}

// Construct a repeated DF2T cascade for reference tests.
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
  BatchBiquadCoeffs c = {};
  for (int stage = 0; stage < kVocoderFilterStages; ++stage) {
    c.coeffs[5 * stage] = b0;
    c.coeffs[5 * stage + 1] = 0.0f;
    c.coeffs[5 * stage + 2] = b2;
    c.coeffs[5 * stage + 3] = -a1;
    c.coeffs[5 * stage + 4] = -a2;
  }
  return c;
}

// Update pCoeffs pointer after the BatchBiquadCoeffs struct is reassigned.
// CMSIS-DSP stores a raw pointer to the coefficients array; call this whenever
// the target BatchBiquadCoeffs object is updated in place so the instance keeps
// pointing at valid memory.
inline void batchBiquadUpdateCoeffs(BatchBiquadState &s,
                                    const BatchBiquadCoeffs &c) {
  s.inst.pCoeffs = c.coeffs;
  s.coefficients = &c;
}

// Reseat pCoeffs and pState pointers without zeroing state.
// Use instead of batchBiquadInit when the filter is already running and you
// only need to point the instance at (possibly-moved) memory.
// batchBiquadInit zeros the state via CMSIS init, causing audible zipper noise
// on live filters (e.g. bandwidth sweeps).
inline void batchBiquadReseat(BatchBiquadState &s, const BatchBiquadCoeffs &c) {
  s.inst.pCoeffs = c.coeffs;
  s.inst.pState  = s.state;
  s.inst.numStages = kVocoderFilterStages;
  s.coefficients = &c;
}

// Process the cascade and apply the envelope once after its final stage.
// The small bounded scratch buffer follows the NT's maximum callback size;
// chunking also keeps this helper valid for larger host-side test buffers.
// gainMix and gainMixComp (= 1 - gainMix) must be pre-computed for the block.
inline void batchBiquadProcessAndAccum(BatchBiquadState &s,
                                       const float *src, float *accum,
                                       int blockSize, float &gainState,
                                       float gainTarget, float gainMix,
                                       float gainMixComp,
                                       float bandGainScale) {
  if (s.coefficients->useSvf) {
    const float *p = s.coefficients->svf;
    // Snapshot coefficients so buffer stores cannot force reloads per sample.
    const float p0 = p[0], p1 = p[1], p2 = p[2], p3 = p[3], p4 = p[4], p5 = p[5], p6 = p[6], p7 = p[7];
    for (int offset = 0; offset < blockSize; offset += 24) {
      const int count = blockSize - offset < 24 ? blockSize - offset : 24;
      float ic10 = s.state[0], ic20 = s.state[1];
      float ic11 = s.state[2], ic21 = s.state[3];
      for (int index = 0; index < count; ++index) {
        const float v30 = src[offset + index] - ic20;
        const float v10 = p0 * ic10 + p1 * v30;
        const float v20 = ic20 + p1 * ic10 + p2 * v30;
        ic10 = 2.0f * v10 - ic10;
        ic20 = 2.0f * v20 - ic20;
        const float firstOutput = p3 * v10;

        const float v31 = firstOutput - ic21;
        const float v11 = p4 * ic11 + p5 * v31;
        const float v21 = ic21 + p5 * ic11 + p6 * v31;
        ic11 = 2.0f * v11 - ic11;
        ic21 = 2.0f * v21 - ic21;
        const float filtered = p7 * v11;

        gainState = gainMix * gainState + gainMixComp * gainTarget;
        accum[offset + index] += filtered * gainState * bandGainScale;
      }
      s.state[0] = ic10 > -1.0e-20f && ic10 < 1.0e-20f ? 0.0f : ic10;
      s.state[1] = ic20 > -1.0e-20f && ic20 < 1.0e-20f ? 0.0f : ic20;
      s.state[2] = ic11 > -1.0e-20f && ic11 < 1.0e-20f ? 0.0f : ic11;
      s.state[3] = ic21 > -1.0e-20f && ic21 < 1.0e-20f ? 0.0f : ic21;
    }
  } else {
    float filtered[24];
    for (int offset = 0; offset < blockSize; offset += 24) {
      const int count = blockSize - offset < 24 ? blockSize - offset : 24;
      batchBiquadProcess(*s.coefficients, s, src + offset, filtered, count);
      for (int index = 0; index < count; ++index) {
        gainState = gainMix * gainState + gainMixComp * gainTarget;
        accum[offset + index] +=
            filtered[index] * gainState * bandGainScale;
      }
    }
  }
  if (gainState >= 0.0f && gainState < 1.0e-20f) gainState = 0.0f;
}

#endif // VOCODER_BATCH_BIQUAD_H
