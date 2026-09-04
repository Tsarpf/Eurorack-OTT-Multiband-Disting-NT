#ifndef VOCODER_ENVELOPE_SHAPE_H
#define VOCODER_ENVELOPE_SHAPE_H

#include <math.h>

// A static approximation to measured Ableton Modulator/Precise Depth curves,
// not a reconstruction of Live's implementation. See tools/live_probe/fit_depth.py.
// The caller supplies an absolute band-envelope reference in NT bus volts.
// It must account for the analysis filter's gain and the host's full-scale
// voltage; a normalized WAV amplitude is not automatically one bus volt.
struct VocoderEnvelopeShape {
  float depth;
  float lowerExponent;
};

inline VocoderEnvelopeShape vocoderMakeEnvelopeShape(float depthPercent) {
  VocoderEnvelopeShape shape = {};
  // The comparisons cover 0..200%. Clamp legacy larger values to that range.
  if (!(depthPercent > 0.0f)) {
    return shape;
  }
  shape.depth = depthPercent >= 200.0f ? 2.0f : depthPercent * 0.01f;
  shape.lowerExponent = shape.depth < 1.0f ? powf(shape.depth, 1.2f) : 1.0f;
  return shape;
}

inline float vocoderEnvelopeDepthGain(const VocoderEnvelopeShape &shape,
                                     float envelopeVolts,
                                     float referenceVolts) {
  if (!(shape.depth > 0.0f)) {
    return 1.0f;
  }
  if (!(referenceVolts > 0.0f) || !isfinite(referenceVolts) ||
      !isfinite(envelopeVolts)) {
    return 0.0f;
  }
  const float positiveEnvelope = envelopeVolts > 0.0f ? envelopeVolts : 0.0f;
  // A purely numerical bound prevents overflow for extreme finite inputs.
  // 1e12 is far outside an audio envelope and is not an operating-level
  // compressor or automatic gain control.
  float x = positiveEnvelope / referenceVolts;
  if (x > 1.0e12f) {
    x = 1.0e12f;
  }
  if (shape.depth == 1.0f) {
    return x;
  }
  if (shape.depth < 1.0f) {
    // Retain some unmodulated carrier below 100%, including at zero envelope.
    // At 0% the early return is exactly unity; at 100% this tends to x.
    const float floor = 0.56f * (1.0f - shape.depth);
    return powf(x + floor, shape.lowerExponent);
  }
  // Above 100%, an affine expansion in a compressed envelope domain gives
  // sustained contrast and progressively rejects quiet bands. It contains no
  // E/average(E) normalization, makeup gain, or secondary envelope follower.
  const float pivot = 5.5f;
  const float compressed = powf(x / pivot, 2.0f / 7.0f);
  const float expanded = 1.0f - shape.depth + shape.depth * compressed;
  if (!(expanded > 0.0f)) {
    return 0.0f;
  }
  // expanded^3.5, avoiding a second general power calculation.
  return pivot * expanded * expanded * expanded * sqrtf(expanded);
}

#endif // VOCODER_ENVELOPE_SHAPE_H
