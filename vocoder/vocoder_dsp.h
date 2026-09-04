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

// Fourth-order Butterworth bandpass: transform a second-order Butterworth
// lowpass prototype, then apply the bilinear transform. Each section has one
// zero at DC and one at Nyquist. The combined band has unity center gain.
// Bandwidth compensation preserves approximately constant fractional digital
// bandwidth instead of pinching the high-frequency bands toward Nyquist.
inline void vocoderCalculateButterworthBandpass(float frequency, float q,
                                                float sampleRate,
                                                float *coefficients,
                                                float *svf = nullptr) {
  const float f = vocoderClamp(frequency, 20.0f, 0.49f * sampleRate);
  const float omega = 2.0f * 3.14159265359f * f / sampleRate;
  const float warpedQ = (q > 0.05f ? q : 0.05f) * sinf(omega) / omega;
  const float center = tanf(0.5f * omega);
  const float bandwidth = center / warpedQ;
  const float centerSquared = center * center;
  const float bandwidthSquared = bandwidth * bandwidth;
  const float halfRoot = 0.353553390593f * bandwidth;
  const float imaginaryRoot = sqrtf(0.5f *
      (hypotf(centerSquared, 0.25f * bandwidthSquared) + centerSquared));
  // Obtain the smaller quadratic root through the product of the roots.
  // Direct subtraction loses precision for very wide bands near Nyquist.
  const float realRoot = 0.125f * bandwidthSquared / imaginaryRoot;
  const float highReal = -halfRoot - realRoot;
  const float highImaginary = halfRoot + imaginaryRoot;
  const float highSquared = highReal * highReal + highImaginary * highImaginary;
  const float lowReal = centerSquared * highReal / highSquared;
  const float lowImaginary = -centerSquared * highImaginary / highSquared;
  const float real[2] = {highReal, lowReal};
  const float imaginary[2] = {highImaginary, lowImaginary};
  for (int stage = 0; stage < 2; ++stage) {
    const float damping = -2.0f * real[stage];
    const float squared = real[stage] * real[stage] + imaginary[stage] * imaginary[stage];
    const float inverse = 1.0f / (1.0f + damping + squared);
    coefficients[5 * stage] = bandwidth * inverse;
    coefficients[5 * stage + 1] = 0.0f;
    coefficients[5 * stage + 2] = -coefficients[5 * stage];
    coefficients[5 * stage + 3] = 2.0f * (1.0f - squared) * inverse;
    coefficients[5 * stage + 4] = -(1.0f - damping + squared) * inverse;
    if (svf) {
      // Trapezoidal state-variable implementation of this analog pole pair.
      // See Andrew Simper, "SvfLinearTrapOptimised2", Cytomic (2013/2016).
      const float g = sqrtf(squared);
      const float k = damping / g;
      const float a1 = 1.0f / (1.0f + g * (g + k));
      svf[4 * stage] = a1;
      svf[4 * stage + 1] = g * a1;
      svf[4 * stage + 2] = g * g * a1;
      svf[4 * stage + 3] = bandwidth / g;
    }
  }
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
  const float knee = threshold > 1.0f ? 1.0f : (1.0f - threshold);
  const float safeKnee = knee > 1.0e-6f ? knee : 1.0e-6f;
  const float compressed = threshold + safeKnee * tanhf((ax - threshold) / safeKnee);
  return x < 0.0f ? -compressed : compressed;
}

inline float vocoderSoftKneeCompress(float x, float knee, float ratio) {
  if (x <= knee) {
    return x;
  }
  const float safeRatio = ratio < 1.0f ? 1.0f : ratio;
  return knee + (x - knee) / safeRatio;
}

inline float vocoderDcBlock(float x, float &x1, float &y1, float r) {
  float y = x - x1 + r * y1;
  if (y > -1.0e-20f && y < 1.0e-20f) y = 0.0f;
  x1 = x;
  y1 = y;
  return y;
}

#endif // VOCODER_DSP_H
