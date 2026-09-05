#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "dsp/filtering_functions.h"   // CMSIS-DSP (resolved via -I$(CMSIS_DSP)/Include)

static const int kOttBands    = 3;
static const int kOttMaxBlock = 64;    // hard ceiling on N passed to step()
static const float kOttMaxUpGainDb = 36.0f;

// ── Scalar math ───────────────────────────────────────────────────────────────

static inline float ottDbToLinear(float db)
{
    return powf(10.0f, db * 0.05f);
}

// Bounded float approximations for the sample-rate gain computer. log2 uses
// the atanh series on a mantissa in [1,2); exp2 uses a sixth-order Taylor
// polynomial on [0,1). This avoids libm in the hot loop on Cortex-M7.
// Inputs to log2 are positive normal floats (the detector is floored first).
inline float ottLog2(float value)
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
         z2 * (1.0f / 7.0f + z2 * (1.0f / 9.0f)))));
}

inline float ottExp2(float value)
{
    value = fmaxf(-120.0f, fminf(120.0f, value));
    int exponent = int(value);
    if (value < float(exponent)) --exponent;
    const float f = value - float(exponent);
    const float polynomial = 1.0f + f * (0.69314718056f + f *
        (0.24022650696f + f * (0.05550410866f + f *
        (0.00961812911f + f * (0.00133335581f + f * 0.00015403530f)))));
    const uint32_t bits = uint32_t(exponent + 127) << 23;
    float scale;
    memcpy(&scale, &bits, sizeof(scale));
    return scale * polynomial;
}

// ── LR4 coefficient computation ───────────────────────────────────────────────
//
// LR4 (Linkwitz-Riley 4th order) = two identical 2nd-order Butterworth biquad
// stages cascaded.  LP + HP sums to unity at all frequencies, making the
// 3-band split perfectly reconstruct the input at 0% compression.
//
// CMSIS DF2T coefficient layout per stage: { b0, b1, b2, a1c, a2c }
// where a1c = -A1_standard, a2c = -A2_standard  (opposite sign convention).
//
// Butterworth 2nd-order via bilinear transform, Q = 1/√2:
//   w  = tan(π·fc/sr)
//   D  = 1 + √2·w + w²
//   LP: b0 = w²/D,  b1 = 2w²/D,  b2 = w²/D
//   HP: b0 = 1/D,   b1 = −2/D,   b2 = 1/D
//   Both: A1 = 2(w²−1)/D,  A2 = (1−√2·w+w²)/D
//   CMSIS: [b0, b1, b2, −A1, −A2]
//
// c10 must point to float[10]; both stages receive identical coefficients.

inline void ottComputeLR4(float fc, float sr, bool hp, float* c10)
{
    const float w   = tanf(3.14159265f * fc / sr);
    const float w2  = w * w;
    const float D   = 1.0f + 1.41421356f * w + w2;
    const float iD  = 1.0f / D;
    const float a1c =  2.0f * (1.0f - w2) * iD;              // −A1 (positive, stable)
    const float a2c = -(1.0f - 1.41421356f * w + w2) * iD;   // −A2 (negative, stable)
    float b0, b1, b2;
    if (!hp) { b0 =  w2 * iD;  b1 =  2.0f * w2 * iD;  b2 = w2 * iD; }
    else     { b0 =  iD;       b1 = -2.0f * iD;        b2 = iD;      }
    for (int st = 0; st < 2; ++st) {
        c10[st*5+0] = b0;
        c10[st*5+1] = b1;
        c10[st*5+2] = b2;
        c10[st*5+3] = a1c;
        c10[st*5+4] = a2c;
    }
}

// LP4 + HP4 at one LR4 crossover reduces to a 2nd-order all-pass.  In a
// three-way tree the low path must receive the upper crossover's all-pass
// phase, otherwise low + (mid + high) is not magnitude-flat between the two
// crossover frequencies.
inline void ottComputeLR4Allpass(float fc, float sr, float* c5)
{
    const float w   = tanf(3.14159265f * fc / sr);
    const float w2  = w * w;
    const float D   = 1.0f + 1.41421356f * w + w2;
    const float iD  = 1.0f / D;
    const float a1c =  2.0f * (1.0f - w2) * iD;
    const float a2c = -(1.0f - 1.41421356f * w + w2) * iD;
    // Standard denominator is {1, -a1c, -a2c}; reverse it for the
    // all-pass numerator, while retaining CMSIS's opposite feedback signs.
    c5[0] = -a2c;
    c5[1] = -a1c;
    c5[2] = 1.0f;
    c5[3] = a1c;
    c5[4] = a2c;
}

// ── Bidirectional gain computer ───────────────────────────────────────────────
//
// Xfer's settled transfer has a hard hinge,
// and Depth scales both compression slopes directly toward 1:1. Upward gain is
// bounded before post/output gain so a vanishing detector level cannot turn
// crossover noise into an arbitrarily large burst.

inline float ottHingeDb(float beyondThresholdDb)
{
    return beyondThresholdDb > 0.0f ? beyondThresholdDb : 0.0f;
}

inline float ottGainDb(float levelDb, float thrDownDb, float thrUpDb,
                      float exDown, float exUp, float depth, bool activeSignal)
{
    const float downDb = -depth * exDown * ottHingeDb(levelDb - thrDownDb);
    const float upDb = activeSignal
        ? fminf(kOttMaxUpGainDb, depth * exUp * ottHingeDb(thrUpDb - levelDb))
        : 0.0f;
    return downDb + upDb;
}

inline void ottGainTargets(float levelDb, float thrDownDb, float thrUpDb,
                           float exDown, float exUp, float depth,
                           float referenceGain, bool activeSignal,
                           float& downGain, float& upGain)
{
    const float downDb = -depth * exDown *
                         ottHingeDb(levelDb - thrDownDb);
    float upCompressionDb = activeSignal
        ? depth * exUp * ottHingeDb(thrUpDb - levelDb)
        : 0.0f;
    if (upCompressionDb > kOttMaxUpGainDb)
        upCompressionDb = kOttMaxUpGainDb;
    downGain = ottDbToLinear(downDb);
    upGain   = referenceGain * ottDbToLinear(upCompressionDb);
}
