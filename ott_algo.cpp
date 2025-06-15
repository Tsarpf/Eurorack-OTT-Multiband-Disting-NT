#include "ott_structs.h"
#include "ott_parameters.h"
#include "ott_dsp.cpp"           // generated Faust DSP

/*──────── requirements / construct / parameterChanged / step ───────*/
static void calculateRequirements(_NT_algorithmRequirements& r, const int32_t*)
{
    MemoryMgr probe(MemoryMgr::Probe);
    FaustDsp::fManager = &probe; FaustDsp::memoryInfo(); FaustDsp::fManager = nullptr;
    r.numParameters = kNumParams;
    r.sram = sizeof(_ottAlgorithm);
    r.dram = probe.total;
    r.dtc  = r.itc = 0;
}

static _NT_algorithm* construct(const _NT_algorithmMemoryPtrs& p,
                                const _NT_algorithmRequirements&, const int32_t*)
{
    auto* a = new (p.sram) _ottAlgorithm();
    a->parameters     = params;
    a->parameterPages = &paramPages;

    MemoryMgr alloc(MemoryMgr::Allocate);
    alloc.base = p.dram;
    FaustDsp::fManager = &alloc;
    a->dsp = new FaustDsp();
    a->dsp->memoryCreate();
    FaustDsp::fManager = nullptr;
    a->dsp->buildUserInterface(&a->ui);

    FaustDsp::classInit(NT_globals.sampleRate);   // static, safe to call many times
    a->dsp->instanceInit(NT_globals.sampleRate);  // per-instance initialisation
    return a;
}

static void parameterChanged(_NT_algorithm* s, int p)
{
    auto* a = (_ottAlgorithm*)s;
    const int16_t v = s->v[p];

    switch (p)
    {
    /* Hi band */
    case kHiDownThr:   a->ui.set("High/DownThr",   0.1f * v); break;
    case kHiUpThr:     a->ui.set("High/UpThr",     0.1f * v); break;
    case kHiDownRat:   a->ui.set("High/DownRat",   0.01f * v); break;
    case kHiUpRat:     a->ui.set("High/UpRat",     0.01f * v); break;
    case kHiMakeup:    a->ui.set("High/Makeup",    0.1f * v); break;

    /* Mid band */
    case kMidDownThr:  a->ui.set("Mid/DownThr",    0.1f * v); break;
    case kMidUpThr:    a->ui.set("Mid/UpThr",      0.1f * v); break;
    case kMidDownRat:  a->ui.set("Mid/DownRat",    0.01f * v); break;
    case kMidUpRat:    a->ui.set("Mid/UpRat",      0.01f * v); break;
    case kMidMakeup:   a->ui.set("Mid/Makeup",     0.1f * v); break;

    /* Low band */
    case kLoDownThr:   a->ui.set("Low/DownThr",    0.1f * v); break;
    case kLoUpThr:     a->ui.set("Low/UpThr",      0.1f * v); break;
    case kLoDownRat:   a->ui.set("Low/DownRat",    0.01f * v); break;
    case kLoUpRat:     a->ui.set("Low/UpRat",      0.01f * v); break;
    case kLoMakeup:    a->ui.set("Low/Makeup",     0.1f * v); break;

    /* X-over & global */
    case kXoverLoMid:
        if (v > s->v[kXoverMidHi]) {
            pushParam(s, kXoverLoMid, s->v[kXoverMidHi]);
            return;
        }
        a->ui.set("Xover/LowMidFreq", v);
        break;
    case kXoverMidHi:
        if (v < s->v[kXoverLoMid]) {
            pushParam(s, kXoverMidHi, s->v[kXoverLoMid]);
            return;
        }
        a->ui.set("Xover/MidHighFreq", v);
        break;
    case kGlobalOut:   a->ui.set("Global/OutGain",   0.1f * v); break;
    case kGlobalWet:   a->ui.set("Global/Wet",       0.01f * v); break;

    default: break;   // routing params have no Faust target
    }
}

/*──────────────────────────  Audio step  ───────────────────────────*/
static void step(_NT_algorithm* s, float* bus, int nfBy4)
{
    auto* a = (_ottAlgorithm*)s;
    const int N = nfBy4 * 4;

    float* inL  = bus + (s->v[kInL]  - 1) * N;
    float* inR  = bus + (s->v[kInR]  - 1) * N;
    float* outL = bus + (s->v[kOutL] - 1) * N;
    float* outR = bus + (s->v[kOutR] - 1) * N;
    bool   replL = s->v[kOutLMode];
    bool   replR = s->v[kOutRMode];

    float* ins[2]  = { inL, inR };
    float* outs[2] = { outL, outR };

    bool bypass = a->state.bypass || s->vIncludingCommon[0];
    if (!bypass) {
        a->dsp->compute(N, ins, outs);
        if (!replL) for (int i=0;i<N;++i) outL[i] += inL[i];
        if (!replR) for (int i=0;i<N;++i) outR[i] += inR[i];
    } else {
        for (int i=0;i<N;++i) {
            outL[i] = inL[i];
            outR[i] = inR[i];
        }
    }
}



/*──────── factory / entry ───────*/
static const _NT_factory factory = {
    NT_MULTICHAR('O','T','T','1'), "OTT MB", "Faust multiband OTT with full UI",
    0,nullptr,nullptr,nullptr,
    calculateRequirements, construct,
    parameterChanged, step, draw,
    nullptr,nullptr,
    kNT_tagUtility,
    hasCustomUi, customUi, setupUi,
    nullptr,nullptr
};
extern "C" uintptr_t pluginEntry(_NT_selector sel,uint32_t d)
{
    switch(sel){case kNT_selector_version:return kNT_apiVersionCurrent;
        case kNT_selector_numFactories:return 1;
        case kNT_selector_factoryInfo:return (uintptr_t)((d==0)?&factory:nullptr);}
    return 0;
}
