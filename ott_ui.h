#pragma once
#include <distingnt/api.h>
#include <cstring>
#include <cmath>

// ── Run-time UI state ─────────────────────────────────────────────────────────

struct UIState {
    enum PotMode { THRESH, RATIO, GAIN } potMode = THRESH;
    enum EncMode { XOVER, GLOBAL }       encMode = XOVER;
};

// ── Host parameter push ───────────────────────────────────────────────────────

inline void pushParam(_NT_algorithm* self, int localIdx, int16_t value)
{
    NT_setParameterFromUi(
        NT_algorithmIndex(self),
        localIdx + NT_parameterOffset(),
        value
    );
}

// ── Forward declarations ──────────────────────────────────────────────────────

uint32_t hasCustomUi(_NT_algorithm*);
void     customUi  (_NT_algorithm*, const _NT_uiData&);
void     setupUi   (_NT_algorithm*, _NT_float3&);
bool     draw      (_NT_algorithm*);

int     mapHzToX      (float hz);
int     mapDownThrToY (float db);
int     mapUpThrToY   (float db);
int     mapGainToY    (float db);
int     mapPercentToY (float p);
int16_t scalePot      (int idx, float pot);

static inline int fast_lrintf(float x)
{
    return (int)((x >= 0.f) ? x + 0.5f : x - 0.5f);
}
