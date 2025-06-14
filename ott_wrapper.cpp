#include <math.h>
#include <new>
#include <cstring>
#include <distingnt/api.h>

#include <faust/dsp/dsp.h>
#include <faust/gui/UI.h>
#include <faust/gui/meta.h>

// Minimal UI collector
struct ParamUI : public UI
{
    struct Entry { const char* label; FAUSTFLOAT* zone; };
    Entry entries[64];
    int count = 0;

    void addEntry(const char* label, FAUSTFLOAT* zone)
    {
        if (count < 64) { entries[count].label = label; entries[count].zone = zone; ++count; }
    }

    // Layouts
    void openTabBox(const char*) override {}
    void openHorizontalBox(const char*) override {}
    void openVerticalBox(const char*) override {}
    void closeBox() override {}

    // Widgets
    void addButton(const char* label, FAUSTFLOAT* zone) override { addEntry(label, zone); }
    void addCheckButton(const char* label, FAUSTFLOAT* zone) override { addEntry(label, zone); }
    void addVerticalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { addEntry(label, zone); }
    void addHorizontalSlider(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { addEntry(label, zone); }
    void addNumEntry(const char* label, FAUSTFLOAT* zone, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT, FAUSTFLOAT) override { addEntry(label, zone); }

    void addHorizontalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addVerticalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addSoundfile(const char*, const char*, Soundfile**) override {}

    void declare(FAUSTFLOAT*, const char*, const char*) override {}

    FAUSTFLOAT getParamValue(const char* name) const
    {
        for (int i = 0; i < count; ++i)
            if (!strcmp(entries[i].label, name))
                return *(entries[i].zone);
        return 0.f;
    }

    void setParamValue(const char* name, FAUSTFLOAT v)
    {
        for (int i = 0; i < count; ++i)
            if (!strcmp(entries[i].label, name)) {
                *(entries[i].zone) = v;
                break;
            }
    }
};

// ─────────────────────────── UI state ───────────────────────────
struct UIState {
    enum PotMode { THRESH, RATIO, GAIN } potMode = THRESH; // BTN-4 cycles
    enum EncMode { XOVER, GLOBAL } encMode = XOVER;       // BTN-3 toggles
    bool bypass = false;                                   // BTN-2
};

//#include "ott_dsp.h"          // class FaustDsp
#include "ott_dsp.cpp"          // class FaustDsp

// ───────────────────────────────────────────────────────────────────
// per-plug-in SRAM object
struct _ottAlgorithm : public _NT_algorithm
{
    FaustDsp dsp;   // the Faust OTT compressor
    ParamUI  ui;    // to control parameters
    UIState  state; // runtime UI state
    uint8_t* dramBase; // pointer to DRAM used by Faust memory manager
};

// Simple memory manager mirroring distingnt's architecture
struct MemoryMgr : public dsp_memory_manager
{
    enum Mode { ClassMem, InstanceMem };

    size_t  total = 0;
    bool    first = true;
    Mode    mode;
    uint8_t* base = nullptr;

    MemoryMgr(Mode m) : mode(m) {}

    void begin(size_t /*count*/) override { total = 0; first = true; }
    void end() override {}

    void info(size_t size, size_t /*reads*/, size_t writes) override
    {
        if (mode == ClassMem) {
            if (writes == 0)
                total += size;
            return;
        }
        if (writes == 0)
            return;
        if (first)
            first = false;
        else
            total += size;
    }

    void* allocate(size_t size) override
    {
        void* ptr = base + total;
        total += size;
        return ptr;
    }

    void destroy(void* /*ptr*/) override {}
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

// ───────────────────────── parameter names ────────────────────────
static const char* kThreshNames[3][2] = {
    {"High/DownThr", "High/UpThr"},
    {"Low/DownThr",  "Low/UpThr"},
    {"Mid/DownThr",  "Mid/UpThr"}
};

static const char* kRatioNames[3][2] = {
    {"High/DownRat", "High/UpRat"},
    {"Low/DownRat",  "Low/UpRat"},
    {"Mid/DownRat",  "Mid/UpRat"}
};

static const char* kGainNames[3] = {
    "High/Makeup",
    "Low/Makeup",
    "Mid/Makeup"
};

// ───────────────── calc / construct ───────────────────────────────
static void calculateRequirements(_NT_algorithmRequirements& r, const int32_t*)
{
    MemoryMgr mem(MemoryMgr::InstanceMem);
    FaustDsp::fManager = &mem;
    FaustDsp::memoryInfo();
    FaustDsp::fManager = nullptr;

    r.numParameters = ARRAY_SIZE(params);
    r.sram = sizeof(_ottAlgorithm);
    r.dram = mem.total;
    r.dtc = 0; r.itc = 0;
}

static _NT_algorithm* construct(const _NT_algorithmMemoryPtrs& p,
                                const _NT_algorithmRequirements&, const int32_t*)
{
    auto* a = new (p.sram) _ottAlgorithm();
    a->parameters = params;
    a->parameterPages = &paramPages;

    MemoryMgr mem(MemoryMgr::InstanceMem);
    mem.base = p.dram;
    mem.total = 0;
    FaustDsp::fManager = &mem;
    a->dsp.memoryCreate();
    FaustDsp::fManager = nullptr;

    a->dsp.buildUserInterface(&a->ui);
    a->dsp.init(NT_globals.sampleRate);
    a->dramBase = p.dram;
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

    if (a->state.bypass)
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
    return kNT_potL | kNT_potC | kNT_potR |
           kNT_encoderL | kNT_encoderR |
           kNT_button1  | kNT_button3 | kNT_button4;
}

static void customUi(_NT_algorithm* s, const _NT_uiData& d)
{
    auto* a = (_ottAlgorithm*)s;
    UIState& ui = a->state;

    /* buttons */
    if ((d.controls & kNT_button1) && !(d.lastButtons & kNT_button1))
        ui.bypass = !ui.bypass;
    if ((d.controls & kNT_button3) && !(d.lastButtons & kNT_button3))
        ui.encMode = (ui.encMode == UIState::XOVER ? UIState::GLOBAL : UIState::XOVER);
    if ((d.controls & kNT_button4) && !(d.lastButtons & kNT_button4))
        ui.potMode = UIState::PotMode((ui.potMode + 1) % 3);

    bool potPressed[3] = {
        (d.controls & kNT_potButtonL),
        (d.controls & kNT_potButtonC),
        (d.controls & kNT_potButtonR)
    };

    for (int p = 0; p < 3; ++p) {
        const char* name = nullptr;
        switch (ui.potMode) {
            case UIState::THRESH: name = kThreshNames[p][potPressed[p]]; break;
            case UIState::RATIO:  name = kRatioNames[p][potPressed[p]];  break;
            case UIState::GAIN:   name = kGainNames[p];                  break;
        }
        if (name) a->ui.setParamValue(name, d.pots[p]);
    }

    /* encoders */
    bool encPressed[2] = {
        (d.controls & kNT_encoderButtonL),
        (d.controls & kNT_encoderButtonR)
    };

    for (int e = 0; e < 2; ++e) {
        const char* name = (ui.encMode == UIState::XOVER)
                           ? (e == 0 ? "Xover/LowMidFreq" : "Xover/MidHighFreq")
                           : (e == 0 ? "Global/OutGain"  : "Global/Wet");
        float step = (ui.encMode == UIState::XOVER)
                     ? (encPressed[e] ? 100.f : 10.f)
                     : (encPressed[e] ? 0.5f  : 0.05f);
        float v = a->ui.getParamValue(name);
        a->ui.setParamValue(name, v + d.encoders[e] * step);
    }
}

static void setupUi(_NT_algorithm* s, _NT_float3& pots)
{
    auto* a = (_ottAlgorithm*)s;
    pots[0] = a->ui.getParamValue(kRatioNames[0][0]);
    pots[1] = a->ui.getParamValue(kRatioNames[1][0]);
    pots[2] = a->ui.getParamValue(kRatioNames[2][0]);
}
// ───────────────── draw: simple status bar ─────────────────────────
static inline int mapHzToX(float hz)
{
    return int((log10f(hz) - 1.f) * 240.f / 3.f);
}

static bool draw(_NT_algorithm* s)
{
    auto* a = (_ottAlgorithm*)s;
    const UIState& ui = a->state;

    std::memset(NT_screen, 0, sizeof(NT_screen));

    const char* hdr = ui.bypass ? "BYPASS" :
                      (ui.potMode == UIState::THRESH ? "THRESH" :
                       ui.potMode == UIState::RATIO  ? "RATIO" : "GAIN");
    NT_drawText(2, 2, hdr);

    int x1 = mapHzToX(a->ui.getParamValue("Xover/LowMidFreq"));
    int x2 = mapHzToX(a->ui.getParamValue("Xover/MidHighFreq"));
    NT_drawShapeI(kNT_line, x1, 10, x1, 50, 8);
    NT_drawShapeI(kNT_line, x2, 10, x2, 50, 8);

    for (int i = 0; i < 3; ++i)
        NT_drawShapeI(kNT_rectangle, 10, 60 + i * 10, 110, 68 + i * 10, 8);

    return false;
}

// ───────────────── factory / entry ─────────────────────────────────
static const _NT_factory factory = {
    NT_MULTICHAR('O','T','T','1'), "Ott MB", "Faust OTT with UI",
    0, nullptr,
    nullptr, nullptr,
    calculateRequirements, construct,
    nullptr, step, draw,
    nullptr, nullptr,
    kNT_tagUtility,
    hasCustomUi, customUi, setupUi,
    nullptr, nullptr
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
