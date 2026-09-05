#ifndef VOCODER_FAST_MATH_H
#define VOCODER_FAST_MATH_H
#include <stdint.h>
#include <string.h>
#include <math.h>

// Bounded float approximations for the sample-rate gain computer. log2 uses
// the atanh series on a mantissa in [1,2); exp2 uses a seventh-order Taylor
// polynomial on [0,1). This avoids libm in the hot loop on Cortex-M7.
// Inputs to log2 are positive normal floats (the detector is floored first).
inline float vocoderLog2(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    const int exponent = int((bits >> 23) & 255) - 127;
    bits = (bits & 0x7fffffu) | 0x3f800000u;
    float mantissa;
    memcpy(&mantissa, &bits, sizeof(mantissa));
    const float z = (mantissa - 1.0f) / (mantissa + 1.0f);
    const float z2 = z * z;
    return float(exponent) + 2.8853900818f * z *
        (1.0f + z2 * (1.0f / 3.0f + z2 * (1.0f / 5.0f +
         z2 * (1.0f / 7.0f + z2 * (1.0f / 9.0f + z2 * (1.0f / 11.0f))))));
}

inline float vocoderExp2(float value)
{
    value = fmaxf(-120.0f, fminf(120.0f, value));
    int exponent = int(value);
    if (value < float(exponent)) --exponent;
    const float f = value - float(exponent);
    const float polynomial = 1.0f + f * (0.69314718056f + f *
        (0.24022650696f + f * (0.05550410866f + f *
        (0.00961812911f + f * (0.00133335581f + f * (0.00015403530f + f * 0.000015252734f))))));
    const uint32_t bits = uint32_t(exponent + 127) << 23;
    float scale;
    memcpy(&scale, &bits, sizeof(scale));
    return scale * polynomial;
}

// Positive audio-envelope powers. Zero is handled before the normal-float
// logarithm; subnormal envelopes are inaudible and treated as zero.
inline float vocoderPositivePower(float x, float exponent) {
  return x >= 1.17549435e-38f ? vocoderExp2(exponent * vocoderLog2(x)) : 0.0f;
}
#endif
