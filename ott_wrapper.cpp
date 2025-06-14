#include <math.h>
#include <new>
#include <distingnt/api.h>

//#include "ott_dsp.h"          // class FaustDsp
#include "ott_dsp.cpp"          // class FaustDsp

// ───────────────────────────────────────────────────────────────────
// per-plug-in SRAM object
struct _ottAlgorithm : public _NT_algorithm
{
    FaustDsp dsp;          // the Faust OTT compressor
    bool     bypass = false;
};

// ───────────────────────────────────────────────────────────────────
// parameters (routing only, like SDK examples)
enum {
    kInL, kInR,
    kOutL, kOutLMode,
    kOutR, kOutRMode,
};

static const _NT_parameter params[] = {
    NT_PARAMETER_AUDIO_INPUT ( "In L", 1, 1 )
    NT_PARAMETER_AUDIO_INPUT ( "In R", 2, 2 )
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Out L", 1, 13 )
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Out R", 2, 14 )
};

static const uint8_t pageRouting[] = {
    kInL, kInR, kOutL, kOutLMode, kOutR, kOutRMode
};
static const _NT_parameterPage pages[] = {
    { "Routing", (uint8_t)ARRAY_SIZE(pageRouting), pageRouting }
};
static const _NT_parameterPages paramPages = {
    (uint8_t)ARRAY_SIZE(pages), pages
};

// ───────────────── calc / construct ───────────────────────────────
static void calculateRequirements(_NT_algorithmRequirements& r, const int32_t*)
{
    r.numParameters = ARRAY_SIZE(params);
    r.sram = sizeof(_ottAlgorithm);
    r.dram = 0; r.dtc = 0; r.itc = 0;
}

static _NT_algorithm* construct(const _NT_algorithmMemoryPtrs& p,
                                const _NT_algorithmRequirements&, const int32_t*)
{
    auto* a = new (p.sram) _ottAlgorithm();
    a->parameters = params;
    a->parameterPages = &paramPages;
    a->dsp.init(NT_globals.sampleRate);
    return a;
}

// ───────────────── audio ──────────────────────────────────────────
static void step(_NT_algorithm* s, float* bus, int nfBy4)
{
    auto* a = (_ottAlgorithm*)s;
    int N  = nfBy4 * 4;
    float* inL  = bus + (s->v[kInL]  - 1) * N;
    float* inR  = bus + (s->v[kInR]  - 1) * N;
    float* outL = bus + (s->v[kOutL] - 1) * N;
    float* outR = bus + (s->v[kOutR] - 1) * N;
    bool   repL =  s->v[kOutLMode];
    bool   repR =  s->v[kOutRMode];

    float* ins[2]  = { inL, inR };
    float* outs[2] = { outL, outR };

    if (a->bypass)
    {
        if (!repL) for (int i=0;i<N;++i) outL[i] += inL[i];
        else       memcpy(outL, inL, N*sizeof(float));
        if (!repR) for (int i=0;i<N;++i) outR[i] += inR[i];
        else       memcpy(outR, inR, N*sizeof(float));
    }
    else
    {
        a->dsp.compute(N, ins, outs);
        if (!repL) for (int i=0;i<N;++i) outL[i] += inL[i];
        if (!repR) for (int i=0;i<N;++i) outR[i] += inR[i];
    }
}

// ───────────────── UI: single centre pot toggles wet/dry ───────────
static uint32_t hasCustomUi(_NT_algorithm*)
{
    return kNT_potC | kNT_button2;
}

static void customUi(_NT_algorithm* s, const _NT_uiData& d)
{
    auto* a = (_ottAlgorithm*)s;
    if (d.controls & kNT_button2) a->bypass = !a->bypass;   // BTN-2 toggle

    // centre pot (index 1) controls wet mix param #17 in Faust
    a->dsp.setParameterValue(17, d.pots[1]);
}

static void setupUi(_NT_algorithm* s, _NT_float3& pots)
{
    pots[1] = ((_ottAlgorithm*)s)->dsp.getParameterValue(17);
}
// ───────────────── draw: simple status bar ─────────────────────────
static bool draw(_NT_algorithm* s)
{
    memset(NT_screen, 0x00, sizeof(NT_screen));
    NT_drawText(2, 2, ((_ottAlgorithm*)s)->bypass ? "BYPASS" : "ACTIVE");
    return false;
}

// ───────────────── factory / entry ─────────────────────────────────
static const _NT_factory factory = {
    NT_MULTICHAR('O','T','T','1'), "Ott MB", "Faust OTT with UI",
    0, calculateRequirements, construct,
    nullptr, step, draw,           // no parameterChanged
    kNT_tagUtility,
    hasCustomUi, customUi, setupUi
};

uintptr_t pluginEntry(_NT_selector sel, uint32_t data)
{
    switch (sel)
    {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)((data==0)?&factory:NULL);
    }
    return 0;
}
