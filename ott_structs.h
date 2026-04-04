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

// First call after coeffs[] are populated — zeros state (correct at startup).
inline void ottLR4Init(OttLR4& f)
{
    arm_biquad_cascade_df2T_init_f32(&f.inst, 2, f.coeffs, f.state);
}

// Update coefficients without touching state — click-free crossover sweeps.
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
};

// ── Cached / derived values ───────────────────────────────────────────────────
// Recomputed in parameterChanged() — never in step().

struct OttCached {
    float relCoeff[kOttBands];    // per-sample release decay coefficient
    float gainSmooth;             // per-sample gain ramp smoother (~5 ms)
    float thrDown[kOttBands];     // downward threshold, linear amplitude
    float thrUp[kOttBands];       // upward threshold,   linear amplitude
    float exDown[kOttBands];      // 1 − 1/ratioDown
    float exUp[kOttBands];        // 1 − 1/ratioUp
    float preGain[kOttBands];     // linear pre-gain
    float postGain[kOttBands];    // linear post-gain
    float outGain;
    float wet;                    // 0..1
};

// ── Runtime state ─────────────────────────────────────────────────────────────

struct OttBands {
    float env[2][kOttBands];        // smoothed block-peak envelope per ch/band
    float gainState[2][kOttBands];  // per-sample gain smoother state
};

struct OttDSPState {
    OttXover  xover;
    OttCached cached;
    OttBands  bands;
    float     sr;
    // Per-block release cache: relCoeff[b]^N, recomputed when N changes
    float     relCoeffPerBlock[kOttBands];
    int       lastBlockN;
};

// ── Per-instance algorithm struct ─────────────────────────────────────────────

struct _ottAlgorithm : public _NT_algorithm {
    OttDSPState dsp;
    UIState     state;
    int         lastParam      = -1;
    int16_t     lastValue      = 0;
    float       potCatch[3]    = {0.f, 0.f, 0.f};
    bool        potCaught[3]   = {false, false, false};
    int         potTarget[3]   = {-1, -1, -1};
    bool        potUpper[3]    = {false, false, false};
};
