/*
 *  OTT multiband compressor for disting NT
 *  – all parameters exposed, custom UI drives them via NT_setParameterFromUi()
 *  – Faust DSP:  ott_dsp.cpp  (generated elsewhere)
 *
 *  Minimal licence header omitted for brevity.
 */

#include <math.h>
#include <new>
#include <cstring>
#include <distingnt/api.h>

#include <faust/dsp/dsp.h>
#include <faust/gui/UI.h>
#include <faust/gui/meta.h>

/*───────────────────────  Minimal Faust UI collector  ───────────────────────*/
struct ParamUI : public UI
{
    struct Entry { const char* label; FAUSTFLOAT* zone; };
    Entry entries[96];
    int   count = 0;

    void addEntry(const char* lbl, FAUSTFLOAT* z)
    { if (count < 96) { entries[count++] = { lbl, z }; } }

    /* layout no-ops */
    void openTabBox      (const char*) override {}
    void openHorizontalBox(const char*) override {}
    void openVerticalBox (const char*) override {}
    void closeBox        ()             override {}

    /* widgets */
    void addButton       (const char* l, FAUSTFLOAT* z) override { addEntry(l,z); }
    void addCheckButton  (const char* l, FAUSTFLOAT* z) override { addEntry(l,z); }
    void addVerticalSlider  (const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { addEntry(l,z); }
    void addHorizontalSlider(const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { addEntry(l,z); }
    void addNumEntry     (const char* l, FAUSTFLOAT* z, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { addEntry(l,z); }

    void addHorizontalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addVerticalBargraph  (const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addSoundfile (const char*, const char*, Soundfile**) override {}
    void declare(FAUSTFLOAT*, const char*, const char*) override {}

    /* helpers */
    FAUSTFLOAT get(const char* n) const
    {
        for (int i = 0; i < count; ++i)
            if (!strcmp(entries[i].label, n)) return *entries[i].zone;
        return 0.f;
    }
    void set(const char* n, FAUSTFLOAT v)
    {
        for (int i = 0; i < count; ++i)
            if (!strcmp(entries[i].label, n)) { *entries[i].zone = v; break; }
    }
};

/*────────────────────────────  UI state flags  ──────────────────────────────*/
struct UIState {
    enum PotMode { THRESH, RATIO, GAIN } potMode = THRESH;   // BTN-4
    enum EncMode { XOVER, GLOBAL } encMode = XOVER;          // BTN-3
    bool bypass = false;                                     // BTN-1
};

/*────────────────────────────  Faust DSP class  ─────────────────────────────
 *  (generated elsewhere; include the cpp so we get one translation unit)   */
#include "ott_dsp.cpp"      // class FaustDsp

/*──────────────────────  Per-instance main struct (in SRAM)  ───────────────*/
struct _ottAlgorithm : public _NT_algorithm
{
    FaustDsp dsp;
    ParamUI  ui;
    UIState  state;

    /* DRAM for Faust heap */
    uint8_t* dramBase = nullptr;
};

/*──────────────────────  Tiny memory manager for Faust  ────────────────────*/
struct MemoryMgr : public dsp_memory_manager
{
    enum Mode { Probe, Allocate };

    Mode    mode;
    size_t  total = 0;
    uint8_t* base = nullptr;

    explicit MemoryMgr(Mode m) : mode(m) {}

    /* probe / allocate phases */
    void begin(size_t) override { total = 0; }
    void end()         override {}

    void info(size_t size, size_t /*r*/, size_t w) override
    {
        if (mode == Probe) { if (w == 0) total += size; }
        /* in Allocate mode info() is irrelevant –  Faust calls allocate() directly */
    }
    void* allocate(size_t size) override
    {
        void* p = base + total;
        total += size;
        return p;
    }
    void destroy(void*) override {}
};

/*───────────────────────────────  Parameters  ──────────────────────────────*/

/*  Stable indices: routing first, then every OTT control.  */
enum {
    /* routing */
    kInL, kInR,
    kOutL, kOutLMode,
    kOutR, kOutRMode,

    /* Hi band */
    kHiDownThr, kHiUpThr,
    kHiDownRat, kHiUpRat,
    kHiMakeup,

    /* Mid band */
    kMidDownThr, kMidUpThr,
    kMidDownRat, kMidUpRat,
    kMidMakeup,

    /* Lo band */
    kLoDownThr, kLoUpThr,
    kLoDownRat, kLoUpRat,
    kLoMakeup,

    /* X-over  + global */
    kXoverLoMid, kXoverMidHi,
    kGlobalOut, kGlobalWet,

    kNumParams
};

/* helper macros – scaling10 means 0.1 dB or 0.1 % steps */
#define P(dbname,min,max,def,unit,sc) { dbname,min,max,def,unit,sc,nullptr }
static const _NT_parameter params[kNumParams] = {
    /* routing */
    NT_PARAMETER_AUDIO_INPUT ("In L", 1, 1)
    NT_PARAMETER_AUDIO_INPUT ("In R", 2, 2)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Out L", 1, 13)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Out R", 2, 14)

    /*  High band  (dB, %, dB) */
    P("Hi/DownThr", -600,   0, -120, kNT_unitDb,       kNT_scaling10),
    P("Hi/UpThr",      0,  400, 120, kNT_unitDb,       kNT_scaling10),
    P("Hi/DownRat",   10, 1000, 500, kNT_unitPercent,  kNT_scaling10),
    P("Hi/UpRat",     10, 8000,2000, kNT_unitPercent,  kNT_scaling10),
    P("Hi/Makeup",  -240,  240,   0, kNT_unitDb,       kNT_scaling10),

    /*  Mid band  */
    P("Mid/DownThr", -600,   0, -120, kNT_unitDb,       kNT_scaling10),
    P("Mid/UpThr",      0,  400, 120, kNT_unitDb,       kNT_scaling10),
    P("Mid/DownRat",   10, 1000, 500, kNT_unitPercent,  kNT_scaling10),
    P("Mid/UpRat",     10, 8000,2000, kNT_unitPercent,  kNT_scaling10),
    P("Mid/Makeup",  -240,  240,   0, kNT_unitDb,       kNT_scaling10),

    /*  Low band  */
    P("Low/DownThr", -600,   0, -120, kNT_unitDb,       kNT_scaling10),
    P("Low/UpThr",      0,  400, 120, kNT_unitDb,       kNT_scaling10),
    P("Low/DownRat",   10, 1000, 500, kNT_unitPercent,  kNT_scaling10),
    P("Low/UpRat",     10, 8000,2000, kNT_unitPercent,  kNT_scaling10),
    P("Low/Makeup",  -240,  240,   0, kNT_unitDb,       kNT_scaling10),

    /*  X-over & global  */
    P("Xover/LoMid",  100, 18000, 400,  kNT_unitHz,      0),
    P("Xover/MidHi",  100, 20000,2500,  kNT_unitHz,      0),
    P("Global/Out",  -240,  240,    0,  kNT_unitDb,      kNT_scaling10),
    P("Global/Wet",     0,  100,  100,  kNT_unitPercent, 0),
};
#undef P

/* simple “Routing” page so there’s at least one menu group */
static const uint8_t pageRouting[] = { kInL,kInR,kOutL,kOutLMode,kOutR,kOutRMode };
static const _NT_parameterPage pages[] = { { "Routing", ARRAY_SIZE(pageRouting), pageRouting } };
static const _NT_parameterPages paramPages = { ARRAY_SIZE(pages), pages };

/*──────────────────────────  Helpers  ───────────────────────────*/

/* push an int16_t value through the host so parameterChanged() fires */
inline void pushParam(_NT_algorithm* self, int32_t localIdx, int16_t v)
{
    NT_setParameterFromUi(
        NT_algorithmIndex(self),
        localIdx + NT_parameterOffset(),   // convert to global index
        v
    );
}

/* convert pot [0-1] to parameter range (int16_t) */
inline int16_t scalePot(int32_t idx, float pot)
{
    const _NT_parameter& p = params[idx];
    return lrintf(p.min + pot * (p.max - p.min));
}

/*──────────────────────────  Requirements  ──────────────────────────*/
static void calculateRequirements(_NT_algorithmRequirements& r, const int32_t*)
{
    /* probe Faust heap size */
    MemoryMgr probe(MemoryMgr::Probe);
    FaustDsp::fManager = &probe;
    FaustDsp::memoryInfo();
    FaustDsp::fManager = nullptr;

    r.numParameters = kNumParams;
    r.sram = sizeof(_ottAlgorithm);
    r.dram = probe.total;
    r.dtc  = r.itc = 0;
}

/*──────────────────────────  Construct  ───────────────────────────*/
static _NT_algorithm* construct(const _NT_algorithmMemoryPtrs& p,
                                const _NT_algorithmRequirements&, const int32_t*)
{
    auto* a = new (p.sram) _ottAlgorithm();
    a->parameters     = params;
    a->parameterPages = &paramPages;

    /* allocate Faust heap in the DRAM block we were given */
    MemoryMgr alloc(MemoryMgr::Allocate);
    alloc.base = p.dram;
    alloc.total = 0;
    FaustDsp::fManager = &alloc;
    a->dsp.memoryCreate();
    FaustDsp::fManager = nullptr;

    a->dsp.buildUserInterface(&a->ui);
    a->dsp.init(NT_globals.sampleRate);
    a->dramBase = p.dram;
    return a;
}

/*──────────────────────────  parameterChanged  ─────────────────────*/
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
    case kXoverLoMid:  a->ui.set("Xover/LowMidFreq", v); break;
    case kXoverMidHi:  a->ui.set("Xover/MidHighFreq", v); break;
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

    if (a->state.bypass)
    {
        if (!replL) for (int i=0;i<N;++i) outL[i] += inL[i];
        else        std::memcpy(outL, inL, N*sizeof(float));
        if (!replR) for (int i=0;i<N;++i) outR[i] += inR[i];
        else        std::memcpy(outR, inR, N*sizeof(float));
    }
    else
    {
        a->dsp.compute(N, ins, outs);
        if (!replL) for (int i=0;i<N;++i) outL[i] += inL[i];
        if (!replR) for (int i=0;i<N;++i) outR[i] += inR[i];
    }
}

/*──────────────────────────  customUi  ───────────────────────────*/
static uint32_t hasCustomUi(_NT_algorithm*)
{
    return kNT_potL | kNT_potC | kNT_potR |
           kNT_encoderL | kNT_encoderR |
           kNT_button1  | kNT_button3 | kNT_button4;
}

static void customUi(_NT_algorithm* s, const _NT_uiData& d)
{
    auto* a  = (_ottAlgorithm*)s;
    UIState& ui = a->state;

    /* buttons */
    if ((d.controls & kNT_button1) && !(d.lastButtons & kNT_button1))
        ui.bypass = !ui.bypass;
    if ((d.controls & kNT_button3) && !(d.lastButtons & kNT_button3))
        ui.encMode = (ui.encMode == UIState::XOVER) ? UIState::GLOBAL : UIState::XOVER;
    if ((d.controls & kNT_button4) && !(d.lastButtons & kNT_button4))
        ui.potMode = UIState::PotMode((ui.potMode + 1) % 3);

    /* pots → three bands */
    const int potTargets[3][3] = {
        { kHiDownThr, kHiDownRat, kHiMakeup },
        { kMidDownThr,kMidDownRat,kMidMakeup },
        { kLoDownThr, kLoDownRat, kLoMakeup }
    };
    const int potTargetsUp[3][3] = {
        { kHiUpThr, kHiUpRat, -1 },
        { kMidUpThr,kMidUpRat,-1 },
        { kLoUpThr, kLoUpRat, -1 }
    };

    for (int p = 0; p < 3; ++p) {
        bool pressed = d.controls & (p==0? kNT_potButtonL : p==1? kNT_potButtonC : kNT_potButtonR);
        int  tgt =
            (ui.potMode == UIState::THRESH) ? (pressed? potTargetsUp[p][0] : potTargets[p][0]) :
            (ui.potMode == UIState::RATIO ) ? (pressed? potTargetsUp[p][1] : potTargets[p][1]) :
                                              potTargets[p][2];
        if (tgt >= 0)
            pushParam(s, tgt, scalePot(tgt, d.pots[p]));
    }

    /* encoders */
    bool encPressL = d.controls & kNT_encoderButtonL;
    bool encPressR = d.controls & kNT_encoderButtonR;

    const int encParam[2] = {
        (ui.encMode == UIState::XOVER) ? kXoverLoMid : kGlobalOut,
        (ui.encMode == UIState::XOVER) ? kXoverMidHi : kGlobalWet
    };
    const float encStep[2] = {
        (ui.encMode == UIState::XOVER) ? (encPressL ? 100.f : 10.f) : (encPressL ? 1.f : 0.1f),
        (ui.encMode == UIState::XOVER) ? (encPressR ? 100.f : 10.f) : (encPressR ? 1.f : 0.1f),
    };

    for (int e=0; e<2; ++e) {
        int idx = encParam[e];
        if (!d.encoders[e]) continue;
        int16_t cur = s->v[idx];
        int16_t inc = lrintf(encStep[e] * d.encoders[e] *
                             ((params[idx].unit == kNT_unitPercent) ? 1.0f
                             : (params[idx].scaling==kNT_scaling10 ? 10.f : 1.f)));
        pushParam(s, idx, cur + inc);
    }
}

/* sync soft-takeover when the algorithm page appears */
static void setupUi(_NT_algorithm* s, _NT_float3& pots)
{
    pots[0] = 0.5f; pots[1] = 0.5f; pots[2] = 0.5f;   // centre the knobs
}

/*──────────────────────────  Draw overlay  ───────────────────────*/
static inline int mapHzToX(float hz)     // 1 kHz → ~40 px, 10 k → ~160 px
{ return int((log10f(hz) - 1.f) * 240.f / 3.f); }

static bool draw(_NT_algorithm* s)
{
    auto* a = (_ottAlgorithm*)s;
    const UIState& ui = a->state;

    std::memset(NT_screen, 0, sizeof(NT_screen));

    /* header text */
    const char* hdr = ui.bypass ? "BYPASS" :
                      ui.potMode == UIState::THRESH ? "THRESH" :
                      ui.potMode == UIState::RATIO  ? "RATIO"  : "GAIN";
    NT_drawText(2, 2, hdr);

    int x1 = mapHzToX(a->ui.get("Xover/LowMidFreq"));
    int x2 = mapHzToX(a->ui.get("Xover/MidHighFreq"));
    NT_drawShapeI(kNT_line, x1, 10, x1, 50, 8);
    NT_drawShapeI(kNT_line, x2, 10, x2, 50, 8);

    return false;   // keep standard parameter strip
}

/*──────────────────────────  Factory / entry  ───────────────────*/
static const _NT_factory factory = {
    NT_MULTICHAR('O','T','T','1'),           /* guid */
    "OTT MB",                                /* name */
    "Faust multiband OTT with full UI",      /* description */
    0, nullptr,                              /* specifications (none) */
    nullptr, nullptr,                        /* static probe/initialise (none) */
    calculateRequirements, construct,
    parameterChanged, step, draw,
    nullptr, nullptr,                        /* MIDI not used */
    kNT_tagUtility,
    hasCustomUi, customUi, setupUi,
    nullptr, nullptr                         /* serialise / deserialise (none) */
};

extern "C" uintptr_t pluginEntry(_NT_selector sel, uint32_t data)
{
    switch (sel) {
        case kNT_selector_version:      return kNT_apiVersionCurrent;
        case kNT_selector_numFactories: return 1;
        case kNT_selector_factoryInfo:  return (uintptr_t)((data==0)? &factory : nullptr);
    }
    return 0;
}
