#pragma once
#include <math.h>
#include <stdint.h>
#include "dsp/filtering_functions.h"   // CMSIS-DSP (resolved via -I$(CMSIS_DSP)/Include)

static const int kOttBands    = 3;
static const int kOttMaxBlock = 64;    // hard ceiling on N passed to step()

// ── Scalar math ───────────────────────────────────────────────────────────────

static inline float ottDbToLinear(float db)
{
    return powf(10.0f, db * 0.05f);
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

// ── Bidirectional gain computer ───────────────────────────────────────────────
//
// Called at block rate — powf is fine here.
//
//  env:     smoothed block-peak envelope (linear amplitude)
//  thrDown: downward threshold (linear) — compress above this
//  thrUp:   upward threshold   (linear) — compress below this
//  exDown:  1 − 1/ratioDown    (0 = no compression, 1 = limiting)
//  exUp:    1 − 1/ratioUp
//
// Downward: when env > thrDown, attenuate toward thrDown.
// Upward:   when env < thrUp,   boost toward thrUp.
// Both can be active simultaneously — that's the OTT character.

inline float ottGain(float env, float thrDown, float thrUp,
                     float exDown, float exUp)
{
    if (env < 1.0e-10f) return 1.0f;
    float g = 1.0f;
    if (env > thrDown && thrDown > 1.0e-10f)
        g *= powf(thrDown / env, exDown);
    if (env < thrUp   && thrUp   > 1.0e-10f)
        g *= powf(thrUp   / env, exUp);
    return g;
}
