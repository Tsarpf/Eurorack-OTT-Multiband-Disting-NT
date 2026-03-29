#ifndef VOCODER_DSP_H
#define VOCODER_DSP_H

#include <math.h>
#include <stdint.h>

static const int kVocoderMaxBands = 40;
static const float kVocoderEpsilon = 1.0e-6f;

inline float vocoderClamp(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

inline float vocoderLerp(float a, float b, float t) { return a + (b - a) * t; }

inline float vocoderSmoothToward(float current, float target, float coeff) {
  return current + coeff * (target - current);
}

inline float vocoderDbFromLinear(float x) {
  const float safe = x < 1.0e-12f ? 1.0e-12f : x;
  return 20.0f * log10f(safe);
}

inline void vocoderCalculateBandpass(float freq, float q, float sampleRate,
                                     float &b0, float &b2, float &a1,
                                     float &a2) {
  const float clampedFreq = vocoderClamp(freq, 20.0f, 0.49f * sampleRate);
  const float safeQ = q < 0.05f ? 0.05f : q;
  const float w0 = 2.0f * 3.14159265359f * clampedFreq / sampleRate;
  const float alpha = sinf(w0) / (2.0f * safeQ);
  const float invA0 = 1.0f / (1.0f + alpha);

  b0 = alpha * invA0;
  b2 = -b0;
  a1 = (-2.0f * cosf(w0)) * invA0;
  a2 = (1.0f - alpha) * invA0;
}

inline float vocoderMixCoeffFromSeconds(float sampleRate, float seconds) {
  const float safeSeconds = seconds < 1.0e-5f ? 1.0e-5f : seconds;
  return expf(-1.0f / (sampleRate * safeSeconds));
}

inline float vocoderTransparentLimit(float x, float threshold) {
  const float ax = fabsf(x);
  if (ax <= threshold) {
    return x;
  }
  const float span = 1.0f - threshold;
  const float compressed =
      threshold + span * tanhf((ax - threshold) / (span > 1.0e-6f ? span : 1.0e-6f));
  return x < 0.0f ? -compressed : compressed;
}

#endif // VOCODER_DSP_H
