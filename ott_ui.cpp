#include "ott_structs.h"
#include "ott_ui.h"
#include "ott_parameters.h"   // enums & params[] (see next section)

/* sync soft-takeover when the algorithm page appears */
void setupUi(_NT_algorithm* s, _NT_float3& pots)
{
    pots[0] = 0.5f; pots[1] = 0.5f; pots[2] = 0.5f;   // centre the knobs
}

bool draw(_NT_algorithm* s)
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

uint32_t hasCustomUi(_NT_algorithm*)
{
    return kNT_potL | kNT_potC | kNT_potR |
           kNT_encoderL | kNT_encoderR |
           kNT_button1  | kNT_button3 | kNT_button4;
}

void customUi(_NT_algorithm* s, const _NT_uiData& d)
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
        int16_t inc = fast_lrintf(encStep[e] * d.encoders[e] *
                             ((params[idx].unit == kNT_unitPercent) ? 1.0f
                             : (params[idx].scaling==kNT_scaling10 ? 10.f : 1.f)));
        pushParam(s, idx, cur + inc);
    }
}
int mapHzToX(float hz)
{
    return int((log10f(hz) - 1.f) * 240.f / 3.f);
}

int16_t scalePot(int idx, float pot)
{
    const _NT_parameter& p = params[idx];
    return fast_lrintf(p.min + pot * (p.max - p.min));
}

