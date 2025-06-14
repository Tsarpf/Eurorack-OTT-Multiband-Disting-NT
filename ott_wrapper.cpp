#include "api.h"      // from distingNT SDK
#include "ott_dsp.h"         // created by Faust when you run the arch script

struct UIState {
    enum PotMode { THRESH=0, RATIO=1, GAIN=2 } potMode = THRESH;   // cycles on BTN‑4
    enum EncMode { XOVER=0, GLOBAL=1 } encMode = XOVER;           // toggles on BTN‑3
    bool bypass = false;
};

class OttPlugin : public PlugInWithCustomUi {
public:
    OttPlugin() { _dsp.init(getSampleRate()); setNumParameters(_dsp.getNumParams()); }

    void processAudio(AudioBuffer &in, AudioBuffer &out) override {
        if (_ui.bypass) { out = in; return; }
        _dsp.compute(in.getNumFrames(), in.getData(), out.getData());
    }

    // --------------------- input handlers ----------------------------
    void onButton(Button b, bool down) override {
        if (!down) return;               // only on press
        switch (b) {
            case BTN_4:  _ui.potMode = UIState::PotMode((_ui.potMode + 1) % 3); break; // cycle pots
            case BTN_3:  _ui.encMode = (_ui.encMode==UIState::XOVER ? UIState::GLOBAL : UIState::XOVER); break;
            case BTN_2:  _ui.bypass = !_ui.bypass; break;                              // bypass toggle
            default:    break; // BTN_1 unused
        }
    }

    void onPot(int idx, float val, bool pushed) override {
        // idx 0..2  → Low / Mid / High band
        const int bandBase = 2 + idx*5;     // first band param index in Faust list
        int p = bandBase;
        switch (_ui.potMode) {
            case UIState::THRESH: p += pushed ? 1 : 0; break; // 0 ↓, 1 ↑
            case UIState::RATIO:  p += 2 + (pushed?1:0); break; // 2 ↓, 3 ↑
            case UIState::GAIN:   p = bandBase + 4; break;     // 4 makeup
        }
        _dsp.setParameterValue(p, val);
    }

    void onEncoder(int idx, int delta, bool pushed) override {
        // idx 0 = left, 1 = right
        int p = (_ui.encMode==UIState::XOVER) ? idx            // 0/1 are crossover freqs
                                              : 17 + idx;      // 17 wet, 18 outGain
        float v = _dsp.getParameterValue(p);
        float step = (_ui.encMode==UIState::XOVER ? (pushed?100:10) : (pushed?0.5:0.05));
        _dsp.setParameterValue(p, v + delta*step);
    }

    // ----------------------- drawing --------------------------------
    void draw(UiContext &ctx) override {
        ctx.clear();
        ctx.setFont(FONT_TINY);
        // header
        ctx.text(2,2, _ui.bypass ? "BYPASS" : (_ui.potMode==UIState::THRESH?"THRESH":(_ui.potMode==UIState::RATIO?"RATIO":"GAIN")) );
        // crossover markers
        int x1 = mapHzToX(_dsp.getParameterValue(0));
        int x2 = mapHzToX(_dsp.getParameterValue(1));
        ctx.drawLine(x1, 10, x1, 50);
        ctx.drawLine(x2, 10, x2, 50);
        // gain‑reduction meters per band (fake; sample env not exposed here)
        for (int i=0;i<3;++i) ctx.fillRect(10, 60+i*10, 100, 8);
    }

private:
    FaustDsp _dsp;
    UIState  _ui;
    int mapHzToX(float hz) { return int((log10f(hz)-1) * 240/3); }
};

REGISTER_PLUGIN(OttPlugin);
