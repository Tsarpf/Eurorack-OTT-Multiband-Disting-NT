// OTT host test — compile with:
//   c++ -std=c++17 -O2 -I distingnt_api/include \
//       -I CMSIS-DSP/Include -I CMSIS-DSP/PrivateInclude \
//       -I CMSIS_6/CMSIS/Core/Include \
//       -DARM_MATH_CM7 -D__FPU_PRESENT=1 \
//       test_ott.cpp \
//       build/host_cmsis_df2t.o build/host_cmsis_df2t_init.o \
//       -o /tmp/test_ott && /tmp/test_ott

#include "distingnt_api/include/distingnt/api.h"
#include "distingnt_api/include/distingnt/serialisation.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

// ── NT host stubs ─────────────────────────────────────────────────────────────

const _NT_globals NT_globals = {
    .sampleRate       = 48000,
    .maxFramesPerStep = 32,
    .workBuffer       = nullptr,
    .workBufferSizeBytes = 0,
};
uint8_t NT_screen[128 * 64];

void NT_drawText(int, int, const char*, int, _NT_textAlignment, _NT_textSize) {}
void NT_drawText(int, int, const char*) {}
void NT_drawShapeI(_NT_shape, int, int, int, int, int) {}
void NT_setParameterFromUi(uint32_t, uint32_t, int16_t) {}
int  NT_algorithmIndex(_NT_algorithm*) { return 0; }
uint32_t NT_parameterOffset(void) { return 0; }
uint32_t NT_getCpuCycleCount(void) { return 0; }
int NT_floatToString(char* buf, float v, int decimals) { return snprintf(buf, 16, "%.*f", decimals, v); }
int NT_intToString(char* buf, int v) { return snprintf(buf, 16, "%d", v); }

void _NT_jsonStream::addMemberName(const char*) {}
void _NT_jsonStream::addNumber(int) {}
void _NT_jsonStream::addNumber(float) {}
bool _NT_jsonParse::numberOfObjectMembers(int& n) { n = 0; return true; }
bool _NT_jsonParse::matchName(const char*) { return false; }
bool _NT_jsonParse::number(int&) { return false; }
bool _NT_jsonParse::skipMember(void) { return true; }

// UI stubs (ott_ui.cpp not compiled in the test)
bool     draw(_NT_algorithm*) { return false; }
uint32_t hasCustomUi(_NT_algorithm*) { return 0; }
void     customUi(_NT_algorithm*, const _NT_uiData&) {}
void     setupUi(_NT_algorithm*, _NT_float3&) {}

// ── Include the DSP (no Faust, no heap) ──────────────────────────────────────
#include "ott_algo.cpp"

// ── Test helpers ──────────────────────────────────────────────────────────────

static void fail(const char* msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

static float rms(const float* buf, int n) {
    float s = 0.f;
    for (int i = 0; i < n; ++i) s += buf[i] * buf[i];
    return sqrtf(s / n);
}

static float dbFS(float linear) { return 20.f * log10f(linear < 1e-12f ? 1e-12f : linear); }

// ── Algorithm setup ───────────────────────────────────────────────────────────

struct OttHost {
    _NT_algorithm* alg;
    std::vector<uint8_t> sram;
    int16_t common[16];
    int16_t v[kNumParams];
};

static OttHost makeOtt() {
    OttHost h = {};

    _NT_algorithmRequirements req;
    factory.calculateRequirements(req, nullptr);
    h.sram.assign(req.sram, 0);
    memset(h.common, 0, sizeof(h.common));
    memset(h.v, 0, sizeof(h.v));

    // Routing: stereo, in 1 (R=2), out 3 (R=4), replace mode
    h.v[kIn]      = 1;
    h.v[kStereo]  = 1;   // stereo: R = L+1
    h.v[kOut]     = 3;
    h.v[kOutMode] = 1;   // replace

    // Default OTT parameters (raw integer values as stored in v[])
    h.v[kHiDownThr]  = -100;  h.v[kHiUpThr]   = -300;
    h.v[kHiDownRat]  =  400;  h.v[kHiUpRat]   =  200;
    h.v[kHiPreGain]  =    0;  h.v[kHiPostGain]=    0;
    h.v[kHiAttack]   =  135;  h.v[kHiRelease] = 1320;

    h.v[kMidDownThr] = -100;  h.v[kMidUpThr]  = -300;
    h.v[kMidDownRat] =  400;  h.v[kMidUpRat]  =  200;
    h.v[kMidPreGain] =    0;  h.v[kMidPostGain]=   0;
    h.v[kMidAttack]  =  224;  h.v[kMidRelease]= 2820;

    h.v[kLoDownThr]  = -100;  h.v[kLoUpThr]   = -300;
    h.v[kLoDownRat]  =  400;  h.v[kLoUpRat]   =  200;
    h.v[kLoPreGain]  =    0;  h.v[kLoPostGain]=    0;
    h.v[kLoAttack]   =  478;  h.v[kLoRelease] = 2820;

    h.v[kXoverLoMid] = 160;
    h.v[kXoverMidHi] = 2500;
    h.v[kGlobalOut]  = 60;   // +6 dB makeup gain (default)
    h.v[kGlobalWet]  = 100;

    _NT_algorithmMemoryPtrs ptrs = { h.sram.data(), nullptr, nullptr, nullptr };
    h.alg = factory.construct(ptrs, req, nullptr);
    h.alg->vIncludingCommon = h.common;
    h.alg->v = h.v;

    for (int p = 0; p < kNumParams; ++p)
        factory.parameterChanged(h.alg, p);

    return h;
}

// Run N_BLOCK blocks of a sine through the OTT. bus channels 1-based.
static void runBlocks(OttHost& h, float* bus, int N, int blocks) {
    for (int b = 0; b < blocks; ++b)
        factory.step(h.alg, bus, N / 4);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_dry_passthrough() {
    // wet=0%: output must equal input exactly
    OttHost h = makeOtt();
    h.v[kGlobalWet] = 0;
    h.v[kGlobalOut] = 0;   // isolate wet=0 path from outGain
    factory.parameterChanged(h.alg, kGlobalWet);
    factory.parameterChanged(h.alg, kGlobalOut);

    const int N = 32;
    std::vector<float> bus(N * 4, 0.f);
    float* inL  = bus.data() + 0 * N;
    float* inR  = bus.data() + 1 * N;
    float* outL = bus.data() + 2 * N;
    float* outR = bus.data() + 3 * N;

    // 1kHz sine at -6 dBFS
    const float amp = 0.5f;
    for (int i = 0; i < N; ++i) {
        float s = amp * sinf(2.f * 3.14159265f * 1000.f * i / 48000.f);
        inL[i] = s;
        inR[i] = s;
    }

    // Several warm-up blocks (filters need to settle)
    for (int b = 0; b < 200; ++b) {
        for (int i = 0; i < N; ++i) {
            float s = amp * sinf(2.f * 3.14159265f * 1000.f * (b * N + i) / 48000.f);
            inL[i] = s;
            inR[i] = s;
        }
        factory.step(h.alg, bus.data(), N / 4);
    }

    float inRms  = rms(inL, N);
    float outRms = rms(outL, N);
    float dbDiff = dbFS(outRms) - dbFS(inRms);

    std::cout << "dry_passthrough: in=" << dbFS(inRms) << " dBFS  out=" << dbFS(outRms)
              << " dBFS  diff=" << dbDiff << " dB\n";

    if (fabsf(dbDiff) > 0.5f)
        fail("wet=0% passthrough has more than 0.5 dB error");
}

static void test_crossover_reconstruction() {
    // No compression (thresholds at 0dB so nothing fires), wet=100%.
    // The 3 bands should sum back to nearly the input.
    OttHost h = makeOtt();
    // Push down thresholds to 0 dBFS so downward never fires
    // Push up thresholds to -60 dBFS so upward never fires on a normal signal
    for (int p : {kHiDownThr, kMidDownThr, kLoDownThr})  { h.v[p] = 0;    factory.parameterChanged(h.alg, p); }
    for (int p : {kHiUpThr,   kMidUpThr,   kLoUpThr})    { h.v[p] = -600; factory.parameterChanged(h.alg, p); }
    h.v[kGlobalOut] = 0;  // isolate crossover from outGain
    factory.parameterChanged(h.alg, kGlobalOut);
    // Ratios don't matter since thresholds never fire, but keep default

    const int N = 32;
    std::vector<float> bus(N * 4, 0.f);
    float* inL  = bus.data() + 0 * N;
    float* outL = bus.data() + 2 * N;
    const float amp = 0.5f;

    // Warm up
    for (int b = 0; b < 500; ++b) {
        for (int i = 0; i < N; ++i) {
            float s = amp * sinf(2.f * 3.14159265f * 1000.f * (b * N + i) / 48000.f);
            inL[i] = s;
            bus.data()[1 * N + i] = s;
        }
        factory.step(h.alg, bus.data(), N / 4);
    }

    float inRms  = rms(inL, N);
    float outRms = rms(outL, N);
    float dbDiff = dbFS(outRms) - dbFS(inRms);

    std::cout << "crossover_reconstruction: in=" << dbFS(inRms) << " dBFS  out=" << dbFS(outRms)
              << " dBFS  diff=" << dbDiff << " dB\n";

    if (fabsf(dbDiff) > 1.0f)
        fail("crossover reconstruction has more than 1 dB error");
}

static void test_default_level() {
    // Default OTT, -12 dBFS sine. Measure level drop.
    OttHost h = makeOtt();

    const int N = 32;
    std::vector<float> bus(N * 4, 0.f);
    float* inL  = bus.data() + 0 * N;
    float* outL = bus.data() + 2 * N;
    const float amp = 0.25f;  // -12 dBFS

    // Warm up 1000 blocks so envelope and gain settle
    for (int b = 0; b < 1000; ++b) {
        for (int i = 0; i < N; ++i) {
            float s = amp * sinf(2.f * 3.14159265f * 1000.f * (b * N + i) / 48000.f);
            inL[i] = s;
            bus.data()[1 * N + i] = s;
        }
        factory.step(h.alg, bus.data(), N / 4);
    }

    float inRms  = rms(inL, N);
    float outRms = rms(outL, N);
    float dbDiff = dbFS(outRms) - dbFS(inRms);

    std::cout << "default_level: in=" << dbFS(inRms) << " dBFS  out=" << dbFS(outRms)
              << " dBFS  diff=" << dbDiff << " dB\n";

    // A -12dBFS signal: thrDown=-10dB so slight downward compression (~1-3dB),
    // thrUp=-30dB so no upward. Should NOT be 20dB down.
    if (dbDiff < -15.f)
        fail("output is more than 15 dB below input at default settings — something is wrong");
}

static void test_levels_across_amplitudes() {
    // Sweep input amplitude and measure steady-state output.
    // Reveals the gain curve in practice.
    const float amps[] = { 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 0.8f, 1.0f };
    const char* labels[] = { "-34", "-26", "-20", "-14", "-6", "-2", "0" };

    const int N = 32;
    std::cout << "\nGain curve (default OTT settings, 1kHz sine):\n";
    std::cout << "  In dBFS  Out dBFS  Gain dB\n";

    for (int ai = 0; ai < 7; ++ai) {
        OttHost h = makeOtt();
        const float amp = amps[ai];
        std::vector<float> bus(N * 4, 0.f);
        float* inL  = bus.data() + 0 * N;
        float* inR  = bus.data() + 1 * N;
        float* outL = bus.data() + 2 * N;
        float* outR = bus.data() + 3 * N;

        // Warm up 2000 blocks to let all envelopes and gain states settle
        for (int b = 0; b < 2000; ++b) {
            for (int i = 0; i < N; ++i) {
                float s = amp * sinf(2.f * 3.14159265f * 1000.f * (b * N + i) / 48000.f);
                inL[i] = inR[i] = s;
            }
            factory.step(h.alg, bus.data(), N / 4);
        }

        float inRms  = rms(inL, N);
        float outRms = rms(outL, N);
        float gainDb = dbFS(outRms) - dbFS(inRms);
        std::cout << "  " << labels[ai] << " dBFS  "
                  << dbFS(outRms) << "  " << gainDb << " dB\n";
    }
}

static void test_band_gains() {
    // Instrument what gain the compressor settles to per band at 0 dBFS.
    // Accesses internal state directly.
    OttHost h = makeOtt();
    const int N = 32;
    std::vector<float> bus(N * 4, 0.f);
    float* inL  = bus.data() + 0 * N;
    float* inR  = bus.data() + 1 * N;
    const float amp = 1.0f;  // 0 dBFS

    for (int b = 0; b < 2000; ++b) {
        for (int i = 0; i < N; ++i) {
            float s = amp * sinf(2.f * 3.14159265f * 1000.f * (b * N + i) / 48000.f);
            inL[i] = inR[i] = s;
        }
        factory.step(h.alg, bus.data(), N / 4);
    }

    const auto* alg = (_ottAlgorithm*)h.alg;
    const OttDSPState& d = alg->dsp;
    const char* bname[] = {"Low","Mid","High"};
    std::cout << "\nPer-band state after 2000 blocks at 0 dBFS:\n";
    for (int b = 0; b < 3; ++b) {
        float envL  = d.bands.env[0][b];
        float gsL   = d.bands.gainState[0][b];
        float tg    = ottGain(envL, d.cached.thrDown[b], d.cached.thrUp[b],
                              d.cached.exDown[b], d.cached.exUp[b]);
        std::cout << "  " << bname[b]
                  << "  env=" << envL << " (" << dbFS(envL) << " dBFS)"
                  << "  gainState=" << gsL << " (" << dbFS(gsL) << " dB)"
                  << "  targetGain=" << tg << " (" << dbFS(tg) << " dB)\n";
    }
    std::cout << "  cached: thrDown[mid]=" << d.cached.thrDown[1]
              << "  thrUp[mid]=" << d.cached.thrUp[1]
              << "  exDown=" << d.cached.exDown[1]
              << "  exUp=" << d.cached.exUp[1] << "\n";
    std::cout << "  wet=" << d.cached.wet
              << "  outGain=" << d.cached.outGain
              << "  gainSmooth=" << d.cached.gainSmooth << "\n";
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== OTT host tests ===\n";
    test_dry_passthrough();
    test_crossover_reconstruction();
    test_default_level();
    test_levels_across_amplitudes();
    test_band_gains();
    std::cout << "\nAll tests passed.\n";
    return 0;
}
