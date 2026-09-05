#pragma once
#include <distingnt/api.h>
#include <string.h>
#include "ott_dsp.h"     // kOttBands, kOttMaxBlock, arm CMSIS header, helpers
#include "ott_ui.h"      // UIState, pushParam, fast_lrintf

// ── LR4 biquad instance ───────────────────────────────────────────────────────
//
// 2-stage cascaded Butterworth (= LR4).  Coefficients and state are owned by
// this struct so CMSIS raw pointers always stay valid.

struct OttLR4 {
    arm_biquad_cascade_df2T_instance_f32 inst;
    float state[4];    // 2 stages × 2 DF2T state vars
    float coeffs[10];  // 2 stages × 5 CMSIS coefficients
};

struct OttAllpass {
    arm_biquad_cascade_df2T_instance_f32 inst;
    float state[2];
    float coeffs[5];
};

// First call after coeffs[] are populated — zeros state (correct at startup).
inline void ottLR4Init(OttLR4& f)
{
    arm_biquad_cascade_df2T_init_f32(&f.inst, 2, f.coeffs, f.state);
}

// Update coefficients without resetting the running filter state.
inline void ottLR4Reseat(OttLR4& f)
{
    f.inst.numStages = 2;
    f.inst.pCoeffs   = f.coeffs;
    f.inst.pState    = f.state;
}

inline void ottLR4Process(OttLR4& f, const float* src, float* dst, int n)
{
    arm_biquad_cascade_df2T_f32(&f.inst, src, dst, (uint32_t)n);
}

inline void ottAllpassInit(OttAllpass& f)
{
    arm_biquad_cascade_df2T_init_f32(&f.inst, 1, f.coeffs, f.state);
}

inline void ottAllpassReseat(OttAllpass& f)
{
    f.inst.numStages = 1;
    f.inst.pCoeffs = f.coeffs;
    f.inst.pState = f.state;
}

inline void ottAllpassProcess(OttAllpass& f, const float* src, float* dst, int n)
{
    arm_biquad_cascade_df2T_f32(&f.inst, src, dst, (uint32_t)n);
}

// ── Crossover bank ────────────────────────────────────────────────────────────
//
// Signal routing (per channel):
//   input → lp1 ──────────────────► lowBand
//   input → hp1 → lp2 ────────────► midBand
//           hp1 → hp2 ────────────► highBand

struct OttXover {
    OttLR4 lp1[2];   // low-mid LP,   per channel
    OttLR4 hp1[2];   // low-mid HP,   per channel
    OttLR4 lp2[2];   // mid-high LP,  per channel (input = hp1 output)
    OttLR4 hp2[2];   // mid-high HP,  per channel (input = hp1 output)
    OttAllpass lowPhase2[2]; // align low with the mid-high LR4 phase
};

// ── Cached / derived values ───────────────────────────────────────────────────
// Recomputed in parameterChanged() — never in step().

struct OttCached {
    float attackCoeff[kOttBands]; // per-sample power averaging coefficient
    float releaseCoeff[kOttBands];// per-sample peak power decay coefficient
    float thrDownDb[kOttBands];   // downward threshold in dB
    float thrUpDb[kOttBands];     // upward threshold in dB
    float exDown[kOttBands];      // 1 − 1/ratioDown
    float exUp[kOttBands];        // 1 − 1/ratioUp
    float preGain[kOttBands];     // linear pre-gain
    float preGainDb[kOttBands];   // detector offset, avoids scaling a buffer pass
    float postGain[kOttBands];    // linear post-gain
    float outGain;
    float depth;                  // 0..1 raw Depth, scales dynamics slopes
    float referenceGain[kOttBands]; // Depth-shaped fixed band balance
};

// ── Runtime state ─────────────────────────────────────────────────────────────

struct OttBands {
    float downGain[2][kOttBands]; // downward gain state, 0..1
    float upGain[2][kOttBands];   // upward/reference gain state, bounded
};

struct OttDSPState {
    OttXover  xover;
    OttCached cached;
    OttBands  bands;
    float     sr;
    float detectorPower[kOttBands]; // RMS detector, per band, stereo linked
    float envelopePower[kOttBands]; // decaying peak of detector power
    float bandDelay[kOttBands][2][2]; // two-sample audio lookahead
};

// ── Per-instance algorithm struct ─────────────────────────────────────────────

struct _ottAlgorithm : public _NT_algorithm {
    OttDSPState dsp;
    UIState     state;
    int         lastParam      = -1;
    int16_t     lastValue      = 0;
    float       potCatch[3]    = {0.f, 0.f, 0.f};
    float       potPrevious[3] = {0.f, 0.f, 0.f};
    bool        potCaught[3]   = {false, false, false};
    bool        potHasPrevious[3] = {false, false, false};
    int         potTarget[3]   = {-1, -1, -1};
    // Each control view remembers its own per-band secondary selection:
    // Threshold/Ratio use Down vs Up, Gain uses Post vs Pre.
    bool        potUpper[UIState::POT_MODE_COUNT][3] = {};
};
