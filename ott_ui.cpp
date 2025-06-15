#include "ott_structs.h"
#include "ott_ui.h"
#include "ott_parameters.h"   // enums & params[] (see next section)
#include <string>

/* sync soft-takeover when the algorithm page appears */
void setupUi(_NT_algorithm* self, _NT_float3& pots)
{
    pots[0] = 0.5f; pots[1] = 0.5f; pots[2] = 0.5f;   // centre the knobs
    auto* a = (_ottAlgorithm*)self;
    for (int i=0;i<3;++i) {
        a->potCaught[i] = false;
        a->potTarget[i] = -1;
        a->potCatch[i] = pots[i];
    }
}

bool draw(_NT_algorithm* self)
{
    auto* a = (_ottAlgorithm*)self;
    const UIState& ui = a->state;

    std::memset(NT_screen, 0, sizeof(NT_screen));

    /* header text */
    bool bypass = a->state.bypass || self->vIncludingCommon[0];
    const char* hdr = bypass ? "BYPASS" :
                      ui.potMode == UIState::THRESH ? "THRESH" :
                      ui.potMode == UIState::RATIO  ? "RATIO"  : "GAIN";
    NT_drawText(2, 20, hdr);

    const char* encMode = ui.encMode == UIState::XOVER ? "XOVER" : "GLOBAL";
    NT_drawText(2, 30, encMode);

    if (a->lastParam >= 0) {
        char buf[40];
        NT_drawText(70, 20, params[a->lastParam].name);
        if (params[a->lastParam].scaling == kNT_scaling10)
            NT_floatToString(buf, a->lastValue * 0.1f, 1);
        else if (params[a->lastParam].unit == kNT_unitPercent)
            NT_floatToString(buf, a->lastValue * 0.01f, 2);
        else
            NT_intToString(buf, a->lastValue);
        NT_drawText(180, 20, buf);
    }

    // Lets draw the values of the pots according to our state
    int16_t hiDownThr = a->ui.get("High/DownThr");

    int x1 = mapHzToX(a->ui.get("Xover/LowMidFreq"));
    int x2 = mapHzToX(a->ui.get("Xover/MidHighFreq"));
    NT_drawShapeI(kNT_line, x1, 10, x1, 50, 8);
    NT_drawShapeI(kNT_line, x2, 10, x2, 50, 8);

    auto drawBand = [&](int xStart, int xEnd, const char* name){
        char key[32];
        snprintf(key, sizeof(key), "%s/DownThr", name);
        float dThr = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/UpThr", name);
        float uThr = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/DownRat", name);
        float dRat = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/UpRat", name);
        float uRat = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/Makeup", name);
        float gain = a->ui.get(key);
        int yDown = mapDbToY(dThr);
        int yUp   = mapDbToY(uThr);
        NT_drawShapeI(kNT_line, xStart, yDown, xEnd, yDown, 10);
        NT_drawShapeI(kNT_line, xStart, yUp,   xEnd, yUp,   10);
        int dTilt = int(5 * dRat);
        int uTilt = int(5 * uRat);
        NT_drawShapeI(kNT_line, xStart, yDown, xEnd, yDown-dTilt, 12);
        NT_drawShapeI(kNT_line, xStart, yUp,   xEnd, yUp+uTilt,   12);
        int xBar = (xStart+xEnd)/2;
        int yGain = mapGainToY(gain);
        NT_drawShapeI(kNT_line, xBar, 50, xBar, yGain, 15);
    };

    drawBand(0, x1, "Low");
    drawBand(x1, x2, "Mid");
    drawBand(x2, 240, "High");

    int xGW = 250;
    int yWet = mapPercentToY(a->ui.get("Global/Wet"));
    int yGain = mapGainToY(a->ui.get("Global/OutGain"));
    NT_drawShapeI(kNT_line, xGW, 50, xGW, yWet, 14);
    NT_drawShapeI(kNT_line, xGW+5, 50, xGW+5, yGain, 14);

    return false;   // keep standard parameter strip
}

uint32_t hasCustomUi(_NT_algorithm*)
{
    return kNT_potL | kNT_potC | kNT_potR |
           kNT_encoderL | kNT_encoderR |
           kNT_button1 | kNT_button3 | kNT_button4 |
           kNT_potButtonL | kNT_potButtonC | kNT_potButtonR |
           kNT_encoderButtonL | kNT_encoderButtonR;
}

void customUi(_NT_algorithm* self, const _NT_uiData& data)
{
    auto* a  = (_ottAlgorithm*)self;
    UIState& ui = a->state;

    /* buttons */
    if ((data.controls & kNT_button1) && !(data.lastButtons & kNT_button1))
        ui.bypass = !ui.bypass;
    if ((data.controls & kNT_button3) && !(data.lastButtons & kNT_button3))
        ui.encMode = (ui.encMode == UIState::XOVER) ? UIState::GLOBAL : UIState::XOVER;
    if ((data.controls & kNT_button4) && !(data.lastButtons & kNT_button4))
        ui.potMode = UIState::PotMode((ui.potMode + 1) % 3);

    /* pots → three bands */
    const int potTargets[3][3] = {
        { kLoDownThr, kLoDownRat, kLoMakeup },
        { kMidDownThr,kMidDownRat,kMidMakeup },
        { kHiDownThr, kHiDownRat, kHiMakeup }
    };
    const int potTargetsUp[3][3] = {
        { kLoUpThr, kLoUpRat, -1 },
        { kMidUpThr,kMidUpRat,-1 },
        { kHiUpThr, kHiUpRat, -1 }
    };

    for (int p = 0; p < 3; ++p) {
        bool pressed = data.controls & (p==0? kNT_potButtonL : p==1? kNT_potButtonC : kNT_potButtonR);
        int  tgt =
            (ui.potMode == UIState::THRESH) ? (pressed? potTargetsUp[p][0] : potTargets[p][0]) :
            (ui.potMode == UIState::RATIO ) ? (pressed? potTargetsUp[p][1] : potTargets[p][1]) :
                                              potTargets[p][2];
        if (tgt >= 0) {
            if (a->potTarget[p] != tgt) {
                a->potTarget[p] = tgt;
                a->potCaught[p] = false;
                a->potCatch[p] = (self->v[tgt] - params[tgt].min) / float(params[tgt].max - params[tgt].min);
            }
            float pos = data.pots[p];
            if (!a->potCaught[p]) {
                if (fabsf(pos - a->potCatch[p]) < 0.05f)
                    a->potCaught[p] = true;
            }
            if (a->potCaught[p])
                pushParam(self, tgt, scalePot(tgt, pos));
        }
    }

    /* encoders */
    bool encPressL = data.controls & kNT_encoderButtonL;
    bool encPressR = data.controls & kNT_encoderButtonR;

    const int encParam[2] = {
        (ui.encMode == UIState::XOVER) ? kXoverLoMid : kGlobalOut,
        (ui.encMode == UIState::XOVER) ? kXoverMidHi : kGlobalWet
    };
    const float encStep[2] = {
        (ui.encMode == UIState::XOVER)
            ? (encPressL ? (self->v[kXoverLoMid] * 0.1f) : 10.f)
            : (encPressL ? 1.f : 0.1f),
        (ui.encMode == UIState::XOVER)
            ? (encPressR ? (self->v[kXoverMidHi] * 0.1f) : 10.f)
            : (encPressR ? 1.f : 0.1f),
    };

    for (int e=0; e<2; ++e) {
        int idx = encParam[e];
        if (!data.encoders[e]) continue;
        int16_t cur = self->v[idx];
        int16_t inc = fast_lrintf(encStep[e] * data.encoders[e] *
                             ((params[idx].unit == kNT_unitPercent) ? 1.0f
                             : (params[idx].scaling==kNT_scaling10 ? 10.f : 1.f)));
        pushParam(self, idx, cur + inc);
    }
}
int mapHzToX(float hz)
{
    return int((log10f(hz) - 1.f) * 240.f / 3.f);
}

int mapDbToY(float db)
{
    return 50 - int(((db + 60.f) / 100.f) * 40.f);
}

int mapGainToY(float db)
{
    return 50 - int(((db + 24.f) / 48.f) * 40.f);
}

int mapPercentToY(float p)
{
    return 50 - int((p / 100.f) * 40.f);
}

int16_t scalePot(int idx, float pot)
{
    const _NT_parameter& p = params[idx];
    return fast_lrintf(p.min + pot * (p.max - p.min));
}

