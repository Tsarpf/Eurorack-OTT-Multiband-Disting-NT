#pragma once
#include "ott_memory.h"
#include <faust/gui/UI.h>
#include <cstring>
#include <cmath>

/* stack-based Faust UI collector (unchanged) */
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

/* run-time UI state */
struct UIState {
    enum PotMode { THRESH, RATIO, GAIN } potMode = THRESH;
    enum EncMode { XOVER, GLOBAL } encMode = XOVER;
    bool bypass = false;
};

/* forward declarations used by ott_algo.cpp */
uint32_t hasCustomUi(_NT_algorithm*);
void     customUi  (_NT_algorithm*, const _NT_uiData&);
void     setupUi   (_NT_algorithm*, _NT_float3&);
bool     draw      (_NT_algorithm*);

int mapHzToX(float hz);
int16_t scalePot(int idx, float p);