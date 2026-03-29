#ifndef VOCODER_FIXED_BIQUAD_H
#define VOCODER_FIXED_BIQUAD_H

#include <stdint.h>

static const int kFixedQShift = 28;
static const int64_t kFixedQOne = (int64_t)1 << kFixedQShift;
static const int64_t kFixedQMax = 0x7fffffffLL;
static const int64_t kFixedQMin = -0x80000000LL;

inline int32_t fixedClamp64To32(int64_t value) {
  if (value > kFixedQMax) {
    return (int32_t)kFixedQMax;
  }
  if (value < kFixedQMin) {
    return (int32_t)kFixedQMin;
  }
  return (int32_t)value;
}

inline int32_t fixedFromFloat(float value) {
  const float scaled = value * (float)kFixedQOne;
  if (scaled >= (float)kFixedQMax) {
    return (int32_t)kFixedQMax;
  }
  if (scaled <= (float)kFixedQMin) {
    return (int32_t)kFixedQMin;
  }
  return (int32_t)scaled;
}

inline float fixedToFloat(int32_t value) {
  return (float)value / (float)kFixedQOne;
}

inline int32_t fixedMulQ28(int32_t a, int32_t b) {
  return fixedClamp64To32(((int64_t)a * (int64_t)b) >> kFixedQShift);
}

struct FixedBiquadQ28 {
  int32_t b0;
  int32_t b2;
  int32_t a1;
  int32_t a2;
};

inline int32_t fixedRunBiquadQ28(const FixedBiquadQ28 &coeffs, int32_t input,
                                 int32_t x2, int32_t &y1, int32_t &y2) {
  const int64_t acc =
      ((int64_t)coeffs.b0 * input) + ((int64_t)coeffs.b2 * x2) -
      ((int64_t)coeffs.a1 * y1) - ((int64_t)coeffs.a2 * y2);
  const int32_t output = fixedClamp64To32(acc >> kFixedQShift);
  y2 = y1;
  y1 = output;
  return output;
}

#endif // VOCODER_FIXED_BIQUAD_H
