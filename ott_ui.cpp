#include "ott_structs.h"
#include "ott_ui.h"
#include "ott_parameters.h"   // enums & params[] (see next section)
#include <string>

static const int kLineStep = 2;   // spacing of ratio lines in pixels
static const int kBandGap  = 5;   // spacing between bands in pixels
static const int kFreqWidth = 220;    // width of frequency display

/* sync soft-takeover when the algorithm page appears */
void setupUi(_NT_algorithm* self, _NT_float3& pots)
{
    auto* a = (_ottAlgorithm*)self;
    plugHeapUse(&a->heap);
    pots[0] = 0.5f; pots[1] = 0.5f; pots[2] = 0.5f;   // centre the knobs
    for (int i=0;i<3;++i) {
        a->potCaught[i] = false;
        a->potTarget[i] = -1;
        a->potCatch[i] = pots[i];
    }
}

bool draw(_NT_algorithm* self)
{
    auto* a = (_ottAlgorithm*)self;
    plugHeapUse(&a->heap);
    const UIState& ui = a->state;

    std::memset(NT_screen, 0, sizeof(NT_screen));

    /* defer text drawing until the end so it appears on top */
    bool bypass = a->state.bypass || self->vIncludingCommon[0];
    const char* hdr = bypass ? "BYPASS" :
                      ui.potMode == UIState::THRESH ? "THRESH" :
                      ui.potMode == UIState::RATIO  ? "RATIO"  : "GAIN";
    const char* encMode = ui.encMode == UIState::XOVER ? "XOVER" : "GLOBAL";
    const char* lastName = nullptr;
    char lastVal[40] = {0};
    if (a->lastParam >= 0) {
        lastName = params[a->lastParam].name;
        if (params[a->lastParam].unit == kNT_unitPercent) {
            if (params[a->lastParam].scaling == kNT_scaling10)
                NT_floatToString(lastVal, a->lastValue * 0.01f, 1);
            else
                NT_floatToString(lastVal, a->lastValue * 0.01f, 2);
        } else if (params[a->lastParam].scaling == kNT_scaling10) {
            NT_floatToString(lastVal, a->lastValue * 0.1f, 1);
        } else {
            NT_intToString(lastVal, a->lastValue);
        }
    }

    // Draw the values of the pots according to our state

    int x1 = mapHzToX(a->ui.get("Xover/LowMidFreq"));
    int x2 = mapHzToX(a->ui.get("Xover/MidHighFreq"));
    NT_drawShapeI(kNT_line, x1, 10, x1, 60, 8);
    NT_drawShapeI(kNT_line, x2, 10, x2, 60, 8);

    auto drawBand = [&](int idx, int xStart, int xEnd, const char* name){
        char key[32];
        snprintf(key, sizeof(key), "%s/DownThr", name);
        float dThr = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/UpThr", name);
        float uThr = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/DownRat", name);
        float dRat = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/UpRat", name);
        float uRat = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/PreGain", name);
        float pre = a->ui.get(key);
        snprintf(key, sizeof(key), "%s/PostGain", name);
        float post = a->ui.get(key);

        int yDown = mapDownThrToY(dThr);
        int yUp   = mapUpThrToY(uThr);

        auto drawBox = [&](int y0, int y1, float ratio, bool fromTop){
            if (y1 <= y0) return;
            float rNorm = ratio <= 1.f ? 0.f : (log10f(ratio) / 3.f);
            if (rNorm > 1.f) rNorm = 1.f;
            int steps = (y1 - y0) / kLineStep;
            float expn = 1.f + rNorm * 4.f;   // cluster lines for strong ratios
            for (int i = 0; i <= steps; ++i) {
                float t = float(i) / float(steps);
                float dist = 1.f - powf(1.f - t, expn);  // 0..1 near threshold
                int y = fromTop ? y0 + int(dist * (y1 - y0))
                                : y1 - int(dist * (y1 - y0));
                float cNorm = dist * dist * rNorm;       // emphasise threshold
                int col = 2 + int(cNorm * 13.f);
                if (col > 15) col = 15;
                NT_drawShapeI(kNT_line, xStart, y, xEnd, y, col);
            }
        };

        drawBox(10,  yDown, dRat, true);   // downward compression from top
        drawBox(yUp, 60,   uRat, false);  // upward compression from bottom

        if (ui.potMode != UIState::GAIN) {
            bool upperSel = a->potUpper[idx];
            int xMid = (xStart + xEnd) / 2;
            if (upperSel)
                NT_drawText(xMid, yUp - 6, "Up", 15, kNT_textCentre, kNT_textTiny);
            else
                NT_drawText(xMid, yDown + 1, "Down", 15, kNT_textCentre, kNT_textTiny);
        }

        int xBar = (xStart + xEnd) / 2;
        int xPre  = xBar - 1;
        int xPost = xBar + 1;
        int yPre  = mapGainToY(pre);
        int yPost = mapGainToY(post);
        NT_drawShapeI(kNT_line, xPre, 60, xPre, yPre, 15);
        NT_drawShapeI(kNT_line, xPost,60, xPost,yPost,15);
        if (ui.potMode == UIState::GAIN) {
            bool upperSel = a->potUpper[idx];
            int xText = upperSel ? xPre : xPost;
            NT_drawText(xText, 62, upperSel?"pre":"post", 15, kNT_textCentre, kNT_textTiny);
        }
    };

    int xGW = 246;
    int xMax = mapHzToX(20000.f);
    if (xMax > xGW - 6)
        xMax = xGW - 6;   // leave room for meters on the right
    int xLoEnd  = x1 - (kBandGap + 1) / 2;
    int xMidSta = x1 + kBandGap / 2;
    int xMidEnd = x2 - (kBandGap + 1) / 2;
    int xHiSta  = x2 + kBandGap / 2;

    drawBand(0,      0,      xLoEnd, "Low");
    drawBand(1,      xMidSta, xMidEnd, "Mid");
    drawBand(2,      xHiSta,  xMax,    "High");

    int yWet = mapPercentToY(a->ui.get("Global/Wet"));
    int yGain = mapGainToY(a->ui.get("Global/OutGain"));
    NT_drawShapeI(kNT_line, xGW, 60, xGW, yWet, 14);
    NT_drawShapeI(kNT_line, xGW+5, 60, xGW+5, yGain, 14);
    NT_drawText(xGW+1, 62, "W", 14, kNT_textLeft, kNT_textTiny);
    NT_drawText(xGW+6, 62, "G", 14, kNT_textLeft, kNT_textTiny);

    /* draw text last so it's always visible */
    NT_drawText(2, 8, hdr);
    NT_drawText(60, 8, encMode);
    if (lastName) {
        NT_drawText(120, 8, lastName);
        NT_drawText(220, 8, lastVal);
    }

    return true;    // suppress standard parameter strip
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
    plugHeapUse(&a->heap);
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
        { kLoDownThr, kLoDownRat, kLoPostGain },
        { kMidDownThr,kMidDownRat,kMidPostGain },
        { kHiDownThr, kHiDownRat, kHiPostGain }
    };
    const int potTargetsUp[3][3] = {
        { kLoUpThr, kLoUpRat, kLoPreGain },
        { kMidUpThr,kMidUpRat,kMidPreGain },
        { kHiUpThr, kHiUpRat, kHiPreGain }
    };

    for (int p = 0; p < 3; ++p) {
        int btn = (p==0? kNT_potButtonL : p==1? kNT_potButtonC : kNT_potButtonR);
        if ((data.controls & btn) && !(data.lastButtons & btn))
            a->potUpper[p] = !a->potUpper[p];
        bool upper = a->potUpper[p];
        int  tgt =
            (ui.potMode == UIState::THRESH) ? (upper? potTargetsUp[p][0] : potTargets[p][0]) :
            (ui.potMode == UIState::RATIO ) ? (upper? potTargetsUp[p][1] : potTargets[p][1]) :
                                             (upper? potTargetsUp[p][2] : potTargets[p][2]);
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
            ? (encPressL ? 10.f : (self->v[kXoverLoMid] * 0.1f))
            : (encPressL ? 1.f : 0.1f),
        (ui.encMode == UIState::XOVER)
            ? (encPressR ? 10.f : (self->v[kXoverMidHi] * 0.1f))
            : (encPressR ? 10.f : 1.f),
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
    return int((log10f(hz) - 1.f) * kFreqWidth / 3.f);
}


int mapDownThrToY(float db)
{
    // map -60..0 dB across the full vertical range 60..10
    float norm = (db + 60.f) / 60.f;        // 0..1
    return 60 - int(norm * 50.f);
}

int mapUpThrToY(float db)
{
    // map -60..0 dB across the full vertical range 60..10
    float norm = (db + 60.f) / 60.f;        // 0..1
    return 60 - int(norm * 50.f);
}

int mapGainToY(float db)
{
    return 60 - int(((db + 24.f) / 48.f) * 50.f);
}

int mapPercentToY(float p)
{
    return 60 - int((p / 100.f) * 50.f);
}

int16_t scalePot(int idx, float pot)
{
    const _NT_parameter& p = params[idx];
    return fast_lrintf(p.min + pot * (p.max - p.min));
}

