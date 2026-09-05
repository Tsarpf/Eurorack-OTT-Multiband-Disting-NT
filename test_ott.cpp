// OTT host test — compile with:
//   c++ -std=c++17 -O2 -I distingnt_api/include \
//       -I CMSIS-DSP/Include -I CMSIS-DSP/PrivateInclude \
//       -D__GNUC_PYTHON__ \
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
static int gParameterPushes = 0;

void NT_drawText(int, int, const char*, int, _NT_textAlignment, _NT_textSize) {}
void NT_drawShapeI(_NT_shape, int, int, int, int, int) {}
void NT_setParameterFromUi(uint32_t, uint32_t, int16_t) { ++gParameterPushes; }
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

// ── Include the native DSP implementation ────────────────────────────────────
#include "ott_algo.cpp"
#include "ott_ui.cpp"

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

static float dbToLinear(float valueDb) { return powf(10.0f, valueDb * 0.05f); }

// ── Algorithm setup ───────────────────────────────────────────────────────────

struct OttHost {
    _NT_algorithm* alg;
    std::vector<uint8_t> sram;
    int16_t common[16];
    int16_t v[kNumParams];
};

static void disableDynamics(OttHost& h);

static OttHost makeOtt() {
    OttHost h = {};

    _NT_algorithmRequirements req;
    factory.calculateRequirements(req, nullptr);
    h.sram.assign(req.sram, 0);
    memset(h.common, 0, sizeof(h.common));
    memset(h.v, 0, sizeof(h.v));

    for (int p = 0; p < kNumParams; ++p)
        h.v[p] = params[p].def;

    // Exercise both channels and keep output separate from the default input.
    h.v[kStereo] = 1;   // right input/output is left + 1
    h.v[kOut] = 3;

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

static void test_depth_zero_reconstruction() {
    // Depth=0 means 1:1 dynamics while retaining the crossover phase.
    // Its steady-state magnitude must remain flat; it is intentionally not a
    // sample-identical raw dry path.
    OttHost h = makeOtt();
    h.v[kGlobalDepth] = 0;
    h.v[kGlobalOut] = 0;   // isolate wet=0 path from outGain
    factory.parameterChanged(h.alg, kGlobalDepth);
    factory.parameterChanged(h.alg, kGlobalOut);

    const int N = 48; // one complete 1 kHz period, independent of filter phase
    std::vector<float> bus(N * 4, 0.f);
    float* inL  = bus.data() + 0 * N;
    float* inR  = bus.data() + 1 * N;
    float* outL = bus.data() + 2 * N;
    float* outR = bus.data() + 3 * N;

    // 1kHz sine at -6 dBFS
    const float amp = 0.1f;
    for (int i = 0; i < N; ++i) {
        float s = amp * sinf(2.f * 3.14159265f * 1000.f * i / 48000.f);
        inL[i] = s;
        inR[i] = s;
    }

    // Several warm-up blocks (filters need to settle)
    for (int b = 0; b < 2000; ++b) {
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

    std::cout << "depth_zero: in=" << dbFS(inRms) << " dBFS  out=" << dbFS(outRms)
              << " dBFS  diff=" << dbDiff << " dB\n";

    if (fabsf(dbDiff) > 1.0f)
        fail("Depth=0 crossover magnitude has more than 1 dB error");
}

static void disableDynamics(OttHost& h) {
    for (int p : {kHiDownThr, kMidDownThr, kLoDownThr}) {
        h.v[p] = 0;
        factory.parameterChanged(h.alg, p);
    }
    for (int p : {kHiUpThr, kMidUpThr, kLoUpThr}) {
        h.v[p] = -600;
        factory.parameterChanged(h.alg, p);
    }
    for (int p : {kHiPreGain, kMidPreGain, kLoPreGain,
                  kHiPostGain, kMidPostGain, kLoPostGain}) {
        h.v[p] = 0;
        factory.parameterChanged(h.alg, p);
    }
}

static void test_crossover_reconstruction() {
    // No compression (thresholds at 0dB so nothing fires), wet=100%.
    // The 3 bands should sum back to nearly the input.
    OttHost h = makeOtt();
    // Push down thresholds to 0 dBFS so downward never fires
    // Push up thresholds to -60 dBFS so upward never fires on a normal signal
    disableDynamics(h);
    h.v[kGlobalDepth] = 0;
    factory.parameterChanged(h.alg, kGlobalDepth);
    h.v[kGlobalOut] = 0;  // isolate crossover from outGain
    factory.parameterChanged(h.alg, kGlobalOut);
    // Ratios don't matter since thresholds never fire, but keep default

    const int N = 48; // one complete 1 kHz period, independent of filter phase
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

    if (!std::isfinite(dbDiff) || dbDiff < -30.f || dbDiff > 40.f)
        fail("default transfer produced an implausible level");
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
        std::cout << "  " << bname[b]
                  << "  down=" << d.bands.downGain[0][b]
                  << " (" << dbFS(d.bands.downGain[0][b]) << " dB)"
                  << "  up=" << d.bands.upGain[0][b]
                  << " (" << dbFS(d.bands.upGain[0][b]) << " dB)\n";
    }
    std::cout << "  cached: thrDown[mid]=" << d.cached.thrDownDb[1]
              << " dB  thrUp[mid]=" << d.cached.thrUpDb[1] << " dB"
              << "  exDown=" << d.cached.exDown[1]
              << "  exUp=" << d.cached.exUp[1] << "\n";
    std::cout << "  depth=" << d.cached.depth
              << "  outGain=" << d.cached.outGain << "\n";
}

static float measureSteadySineGain(float frequency, float peakDb, int depth) {
    OttHost h = makeOtt();
    h.v[kGlobalDepth] = (int16_t)depth;
    factory.parameterChanged(h.alg, kGlobalDepth);
    const int N = 32;
    const int settleBlocks = 3200;  // >2 s at 48 kHz
    const int measureBlocks = 300;  // 0.2 s, coherent for all probe tones
    std::vector<float> bus(N * 4, 0.f);
    const float amplitude = dbToLinear(peakDb);
    double inputSquares = 0.0;
    double outputSquares = 0.0;
    for (int block = 0; block < settleBlocks + measureBlocks; ++block) {
        for (int i = 0; i < N; ++i) {
            const double phase = 2.0 * 3.14159265358979323846 *
                                 (double)frequency * (block * N + i) / 48000.0;
            const float sample = amplitude * (float)sin(phase);
            bus[i] = bus[N + i] = sample;
        }
        factory.step(h.alg, bus.data(), N / 4);
        if (block >= settleBlocks) {
            for (int i = 0; i < N; ++i) {
                inputSquares += (double)bus[i] * bus[i];
                outputSquares += (double)bus[2 * N + i] * bus[2 * N + i];
            }
        }
    }
    return 10.0f * log10f((float)(outputSquares / inputSquares));
}

static void test_xfer_reference_transfer() {
    struct ReferencePoint {
        float frequency;
        float peakDb;
        int depth;
        float expectedGainDb;
    };
    static const ReferencePoint points[] = {
        {40.f, -120.f, 100, 49.79f}, {40.f, -35.f, 100, 12.44f},
        {40.f,  -20.f, 100, -1.98f}, {40.f,   0.f, 100, -18.14f},
        {500.f,-120.f, 100, 45.35f}, {500.f,-35.f, 100, 11.17f},
        {500.f, -20.f, 100, -1.78f}, {500.f,  0.f, 100, -20.05f},
        {8000.f,-120.f,100, 49.91f}, {8000.f,-35.f,100, 10.75f},
        {8000.f, -20.f,100, -3.68f}, {8000.f,  0.f,100, -22.27f},
        {500.f, -35.f, 25, 2.93f},   {500.f, -20.f, 25, 0.14f},
        {500.f, -35.f, 50, 5.55f},   {500.f, -20.f, 50, -0.48f},
        {500.f, -20.f,  0, 0.0f},
    };
    std::cout << "\nXfer OTT 1.37 transfer comparison:\n";
    float worstError = 0.0f;
    for (const ReferencePoint& point : points) {
        const float actual = measureSteadySineGain(point.frequency, point.peakDb,
                                                    point.depth);
        const float error = actual - point.expectedGainDb;
        if (fabsf(error) > worstError)
            worstError = fabsf(error);
        std::cout << "  " << point.frequency << " Hz  in " << point.peakDb
                  << "  D" << point.depth << "  native " << actual
                  << "  ref " << point.expectedGainDb << "  err " << error
                  << " dB\n";
    }
    if (worstError > 0.4f)
        fail("default transfer differs from Xfer reference by more than 0.4 dB");
}

static void test_detector_timebase_and_block_size() {
    // Exercise the whole audio path across callback boundaries, including
    // lookahead, envelope decay and quiet-to-loud transitions.
    auto render = [](int N) {
        OttHost h = makeOtt();
        std::vector<float> output(96000), bus(N * 4, 0.f);
        for (int start = 0; start < 96000; start += N) {
            for (int i = 0; i < N; ++i) {
                const int t = start + i;
                const double amplitude = t < 48000 ? .003 : .4;
                const float x = float(amplitude * sin(2.0 * 3.141592653589793 * 500.0 * t / 48000));
                bus[i] = bus[N + i] = x;
            }
            factory.step(h.alg, bus.data(), N / 4);
            memcpy(output.data() + start, bus.data() + 2*N, N*sizeof(float));
        }
        return output;
    };
    const auto reference = render(4);
    for (int n : {16, 32, 48, 64}) {
        const auto actual = render(n);
        double error = 0, signal = 0;
        for (int i = 0; i < 96000; ++i) {
            const double diff = actual[i] - reference[i];
            error += diff * diff;
            signal += double(reference[i]) * reference[i];
        }
        if (sqrt(error / signal) > 1.0e-4)
            fail("OTT envelope or lookahead depends on callback size");
    }
}

#include "fixtures/ott_dynamics_reference.h"

static void test_xfer_dynamic_reference() {
    for (const auto& ref : kOttDynamicReferences) {
        std::vector<double> source(ref.frames, 0.0);
        double peak = 0;
        for (int i = 0; i < ref.frames; ++i) {
            const double t = double(i) / 48000;
            if (ref.hz) {
                const int level = (i / 48000) % 2 ? -8 : -50;
                source[i] = float(sin(2.0 * 3.141592653589793 * ref.hz * t)) *
                            float(pow(10.0, double(level) / 20));
            } else {
                double tone = 0, env = 0;
                for (int harmonic = 1; harmonic <= 20; ++harmonic)
                    tone += sin(2.0 * 3.141592653589793 * 110 * harmonic * t) / harmonic;
                for (int note = 0; note < 8; ++note) {
                    const double dt = t - .25 - note * .5;
                    if (dt >= 0) env += (1 - exp(-dt / .002)) * exp(-dt / .12);
                }
                source[i] = tone * env;
            }
            peak = fmax(peak, fabs(source[i]));
        }
        if (!ref.hz)
            for (double& sample : source) sample *= .35 / peak;
        OttHost h = makeOtt();
        h.v[kGlobalDepth] = ref.depth;
        factory.parameterChanged(h.alg, kGlobalDepth);
        constexpr int N = 32;
        std::vector<float> bus(N * 4, 0.f), output(ref.frames);
        float outputPeak = 0;
        for (int start = 0; start < ref.frames; start += N) {
            for (int i = 0; i < N; ++i)
                bus[i] = bus[N+i] = float(source[start+i]);
            factory.step(h.alg, bus.data(), N/4);
            for (int i = 0; i < N; ++i) {
                output[start+i] = bus[2*N+i];
                outputPeak = fmaxf(outputPeak, fabsf(bus[2*N+i]));
            }
        }
        float worst = 0;
        for (const auto& window : ref.windows) {
            double squares = 0;
            for (int i = window.start; i < window.end; ++i)
                squares += double(output[i]) * output[i];
            const float actual = float(sqrt(squares / (window.end - window.start)));
            worst = fmaxf(worst, fabsf(dbFS(actual / window.rms)));
        }
        const float peakError = dbFS(outputPeak / ref.peak);
        std::cout << "dynamic_reference " << ref.name << ": worst window=" << worst
                  << " dB, peak error=" << peakError << " dB\n";
        // Largest measured residual is 0.77 dB during the 500 Hz downward
        // step recovery. Keep that bounded while rejecting the old >20 dB errors.
        if (worst > .9f || fabsf(peakError) > .5f)
            fail("OTT transient envelope differs from measured Xfer reference");
    }
}

static void test_gain_math_accuracy() {
    // Cover the actual detector floor and wide pre/post/threshold ranges.
    for (int i = 0; i <= 20000; ++i) {
        const float exponent = -66.0f + i * (86.0f / 20000.0f);
        const float power = exp2f(exponent);
        if (fabsf(ottLog2(power) - log2f(power)) > 1.0e-5f)
            fail("fast detector logarithm is inaccurate");
        const float gainExponent = -100.0f + i * (110.0f / 20000.0f);
        const float exact = exp2f(gainExponent);
        if (fabsf(ottExp2(gainExponent) / exact - 1.0f) > 1.0e-5f)
            fail("fast gain exponential is inaccurate");
    }
}

static void test_silence_does_not_charge_upward_gain() {
    OttHost h = makeOtt();
    const int N = 32;
    std::vector<float> bus(N * 4, 0.f);
    for (int block = 0; block < 15000; ++block)
        factory.step(h.alg, bus.data(), N / 4);
    const auto* a = (_ottAlgorithm*)h.alg;
    for (int band = 0; band < kOttBands; ++band) {
        const float upwardDb = dbFS(a->dsp.bands.upGain[0][band]);
        const float referenceDb = dbFS(a->dsp.cached.referenceGain[band]);
        if (fabsf(upwardDb - referenceDb) > 0.01f)
            fail("silence charged the upward compressor above reference gain");
    }
}

static void test_depth_law_and_upward_bound() {
    float downFull, upFull, downHalf, upHalf, downZero, upZero;
    ottGainTargets(-80.0f, -30.0f, -40.0f, 0.75f, 0.75f, 1.0f, 1.0f, true,
                   downFull, upFull);
    ottGainTargets(-80.0f, -30.0f, -40.0f, 0.75f, 0.75f, 0.5f, 1.0f, true,
                   downHalf, upHalf);
    ottGainTargets(-80.0f, -30.0f, -40.0f, 0.75f, 0.75f, 0.0f, 1.0f, true,
                   downZero, upZero);

    if (fabsf(dbFS(upHalf) - 0.5f * dbFS(upFull)) > 0.01f)
        fail("Depth does not scale the upward gain curve in dB");
    if (fabsf(downZero - 1.0f) > 1.0e-6f ||
        fabsf(upZero - 1.0f) > 1.0e-6f)
        fail("Depth=0 does not produce 1:1 dynamics");

    float boundedDown, boundedUp;
    ottGainTargets(-200.0f, -30.0f, -40.0f, 0.99f, 0.99f, 1.0f, 1.0f, true,
                   boundedDown, boundedUp);
    if (dbFS(boundedUp) > kOttMaxUpGainDb + 0.01f)
        fail("upward gain exceeds its stability bound");
}

static void test_xfer_static_law_calibration() {
    static const int downRatioParameters[kOttBands] = {
        kLoDownRat, kMidDownRat, kHiDownRat
    };
    static const int upRatioParameters[kOttBands] = {
        kLoUpRat, kMidUpRat, kHiUpRat
    };
    static const int downThresholdParameters[kOttBands] = {
        kLoDownThr, kMidDownThr, kHiDownThr
    };
    static const int upThresholdParameters[kOttBands] = {
        kLoUpThr, kMidUpThr, kHiUpThr
    };
    static const int expectedDownRatioRaw[kOttBands] = {32767, 10000, 10000};
    static const int expectedUpRatioRaw[kOttBands] = {400, 400, 400};
    static const int expectedDownThresholdRaw[kOttBands] = {-355, -317, -369};
    static const int expectedUpThresholdRaw[kOttBands] = {-425, -433, -422};

    // Preserve the original hundredths raw units for preset compatibility.
    // Xfer's nominal 1000:1 low-band default saturates at int16's 327.67:1.
    for (int band = 0; band < kOttBands; ++band) {
        const _NT_parameter& down = params[downRatioParameters[band]];
        const _NT_parameter& up = params[upRatioParameters[band]];
        if (down.scaling != kNT_scaling100 || up.scaling != kNT_scaling100 ||
            down.min != 100 || down.max != 32767 ||
            up.min != 100 || up.max != 32767 ||
            down.def != expectedDownRatioRaw[band] ||
            up.def != expectedUpRatioRaw[band])
            fail("ratio parameter scaling/defaults no longer match Xfer fit");
        if (params[downThresholdParameters[band]].def !=
                expectedDownThresholdRaw[band] ||
            params[upThresholdParameters[band]].def !=
                expectedUpThresholdRaw[band])
            fail("threshold defaults no longer match Xfer detector calibration");
    }

    OttHost h = makeOtt();
    auto* a = (_ottAlgorithm*)h.alg;
    static const float expectedDownEx[kOttBands] = {
        1.0f - 100.0f / 32767.0f, 0.99f, 0.99f
    };
    for (int band = 0; band < kOttBands; ++band) {
        if (fabsf(a->dsp.cached.exDown[band] - expectedDownEx[band]) > 1.0e-6f ||
            fabsf(a->dsp.cached.exUp[band] - 0.75f) > 1.0e-6f)
            fail("cached ratio exponent does not use hundredths scaling");
    }

    struct DepthCase {
        int percent;
        float thresholdDeltaDb;
        float lowHighReferenceGain;
        float midReferenceGain;
    };
    static const DepthCase cases[] = {
        {25, 2.5752204f, 1.7721642f, 1.3920397f},
        {50, 1.6293603f, 2.6920490f, 1.8443553f},
        {100, 0.0f,       4.9749810f, 2.9298150f},
    };
    for (const DepthCase& depthCase : cases) {
        h.v[kGlobalDepth] = (int16_t)depthCase.percent;
        factory.parameterChanged(h.alg, kGlobalDepth);
        const OttCached& cached = a->dsp.cached;
        const float depth = depthCase.percent * 0.01f;
        if (fabsf(cached.depth - depth) > 1.0e-7f)
            fail("dynamics do not use raw Depth");
        if (fabsf(cached.referenceGain[0] -
                  depthCase.lowHighReferenceGain) > 2.0e-6f ||
            fabsf(cached.referenceGain[1] -
                  depthCase.midReferenceGain) > 2.0e-6f ||
            fabsf(cached.referenceGain[2] -
                  depthCase.lowHighReferenceGain) > 2.0e-6f)
            fail("per-band Depth-shaped reference gain changed");
        for (int band = 0; band < kOttBands; ++band) {
            const float expectedDown =
                expectedDownThresholdRaw[band] * 0.1f +
                depthCase.thresholdDeltaDb;
            const float expectedUp =
                expectedUpThresholdRaw[band] * 0.1f +
                depthCase.thresholdDeltaDb;
            if (fabsf(cached.thrDownDb[band] - expectedDown) > 2.0e-5f ||
                fabsf(cached.thrUpDb[band] - expectedUp) > 2.0e-5f)
                fail("partial-Depth threshold shift changed");
        }
    }

    // Xfer's settled branch transfer has a hard hinge, not a soft knee.
    if (ottHingeDb(-0.001f) != 0.0f || ottHingeDb(0.0f) != 0.0f ||
        fabsf(ottHingeDb(0.001f) - 0.001f) > 1.0e-9f)
        fail("dynamics hinge is no longer hard");
}

static void test_attack_controls_gain_motion() {
    OttHost fast = makeOtt();
    OttHost slow = makeOtt();
    fast.v[kMidAttack] = 1;       // 0.1 ms
    slow.v[kMidAttack] = 5000;    // 500 ms
    factory.parameterChanged(fast.alg, kMidAttack);
    factory.parameterChanged(slow.alg, kMidAttack);

    const int N = 32;
    std::vector<float> fastBus(N * 4, 0.f), slowBus(N * 4, 0.f);
    for (int block = 0; block < 20; ++block) {
        for (int i = 0; i < N; ++i) {
            const float sample = 0.8f * sinf(2.f * 3.14159265f * 500.f *
                                             (block * N + i) / 48000.f);
            fastBus[i] = fastBus[N + i] = sample;
            slowBus[i] = slowBus[N + i] = sample;
        }
        factory.step(fast.alg, fastBus.data(), N / 4);
        factory.step(slow.alg, slowBus.data(), N / 4);
    }

    const auto* fastAlg = (_ottAlgorithm*)fast.alg;
    const auto* slowAlg = (_ottAlgorithm*)slow.alg;
    if (!(fastAlg->dsp.bands.downGain[0][1] <
          slowAlg->dsp.bands.downGain[0][1] - 0.05f))
        fail("Mid Attack does not control downward gain motion");
}

static void test_mono_keeps_right_state_current() {
    OttHost h = makeOtt();
    const int N = 32;
    std::vector<float> bus(N * 4, 0.f);

    for (int block = 0; block < 200; ++block) {
        for (int i = 0; i < N; ++i) {
            const float sample = 0.1f * sinf(2.f * 3.14159265f * 500.f *
                                             (block * N + i) / 48000.f);
            bus[i] = bus[N + i] = sample;
        }
        factory.step(h.alg, bus.data(), N / 4);
    }

    h.v[kStereo] = 0;
    factory.parameterChanged(h.alg, kStereo);
    for (int block = 0; block < 1500; ++block) {
        for (int i = 0; i < N; ++i)
            bus[i] = 0.4f * sinf(2.f * 3.14159265f * 2000.f *
                                  (block * N + i) / 48000.f);
        factory.step(h.alg, bus.data(), N / 4);
    }

    h.v[kStereo] = 1;
    factory.parameterChanged(h.alg, kStereo);
    for (int i = 0; i < N; ++i) {
        const float sample = 0.4f * sinf(2.f * 3.14159265f * 2000.f * i /
                                         48000.f);
        bus[i] = bus[N + i] = sample;
    }
    factory.step(h.alg, bus.data(), N / 4);
    const float left = rms(bus.data() + 2 * N, N);
    const float right = rms(bus.data() + 3 * N, N);
    if (fabsf(dbFS(right) - dbFS(left)) > 1.0f)
        fail("stereo re-enable exposed stale right-channel DSP state");
}

static void test_mono_input_fallback_keeps_stereo_outputs_equal() {
    OttHost h = makeOtt();
    h.v[kGlobalDepth] = 0;
    factory.parameterChanged(h.alg, kGlobalDepth);
    const int N = 32;
    std::vector<float> bus(kNT_lastBus * N, 0.0f);

    // First make the two crossover histories deliberately different.
    for (int block = 0; block < 300; ++block) {
        for (int i = 0; i < N; ++i) {
            bus[i] = 0.8f * sinf(2.f * 3.14159265f * 200.f *
                                  (block * N + i) / 48000.f);
            bus[N + i] = 0.8f * sinf(2.f * 3.14159265f * 8000.f *
                                      (block * N + i) / 48000.f);
        }
        factory.step(h.alg, bus.data(), N / 4);
    }

    // The final bus has no +1 partner, so Stereo duplicates this mono input to
    // both available output buses. No old right-channel history may leak out.
    h.v[kIn] = kNT_lastBus;
    for (int i = 0; i < N; ++i)
        bus[(kNT_lastBus - 1) * N + i] = 0.2f;
    factory.step(h.alg, bus.data(), N / 4);
    for (int i = 0; i < N; ++i)
        if (fabsf(bus[2 * N + i] - bus[3 * N + i]) > 1.0e-6f)
            fail("mono input fallback exposed stale right-channel state");
}

static void test_bypass_routing() {
    OttHost h = makeOtt();
    h.common[0] = 1;
    h.v[kIn] = 1;
    h.v[kOut] = 2; // overlaps the right input in stereo mode
    h.v[kOutMode] = 1;
    const int N = 32;
    std::vector<float> bus(N * 4, 0.f);
    for (int i = 0; i < N; ++i) {
        bus[i] = 0.1f + i * 0.001f;
        bus[N + i] = -0.2f - i * 0.002f;
    }
    const std::vector<float> original = bus;
    factory.step(h.alg, bus.data(), N / 4);
    for (int i = 0; i < N; ++i) {
        if (fabsf(bus[N + i] - original[i]) > 1.0e-7f ||
            fabsf(bus[2 * N + i] - original[N + i]) > 1.0e-7f)
            fail("overlapping stereo bypass routing corrupted a channel");
    }

    h.v[kOut] = 3;
    h.v[kOutMode] = 0;
    std::fill(bus.begin(), bus.end(), 0.25f);
    factory.step(h.alg, bus.data(), N / 4);
    for (int i = 0; i < N; ++i) {
        if (fabsf(bus[2 * N + i] - 0.5f) > 1.0e-7f ||
            fabsf(bus[3 * N + i] - 0.5f) > 1.0e-7f)
            fail("bypass ignored Add output mode");
    }

    // Every selectable bus can be the left side of a stereo input pair except
    // the final bus. Do not silently collapse high-numbered pairs to mono.
    h.v[kIn] = kNT_lastBus - 1;
    h.v[kOut] = 1;
    h.v[kOutMode] = 1;
    std::vector<float> highBus(kNT_lastBus * N, 0.0f);
    for (int i = 0; i < N; ++i) {
        highBus[(kNT_lastBus - 2) * N + i] = 0.3f + i * 0.001f;
        highBus[(kNT_lastBus - 1) * N + i] = -0.4f - i * 0.001f;
    }
    factory.step(h.alg, highBus.data(), N / 4);
    for (int i = 0; i < N; ++i) {
        if (fabsf(highBus[i] - (0.3f + i * 0.001f)) > 1.0e-7f ||
            fabsf(highBus[N + i] - (-0.4f - i * 0.001f)) > 1.0e-7f)
            fail("high-numbered stereo input pair collapsed to mono");
    }
}

static void test_stereo_dynamics_are_linked() {
    OttHost h = makeOtt();
    const int N = 48;
    std::vector<float> bus(N * 4, 0.f);
    for (int block = 0; block < 2000; ++block) {
        for (int i = 0; i < N; ++i) {
            const float phase = 2.f * 3.14159265f * 500.f *
                                (block * N + i) / 48000.f;
            bus[i] = 0.001f * sinf(phase);
            bus[N + i] = 1.0f * sinf(phase);
        }
        factory.step(h.alg, bus.data(), N / 4);
    }
    const float leftGain = dbFS(rms(bus.data() + 2 * N, N)) -
                           dbFS(rms(bus.data(), N));
    const float rightGain = dbFS(rms(bus.data() + 3 * N, N)) -
                            dbFS(rms(bus.data() + N, N));
    if (fabsf(leftGain - rightGain) > 0.1f)
        fail("stereo dynamics changed the left/right image");
}

static void pressUiButton(_NT_algorithm* alg, uint16_t button) {
    _NT_uiData data = {};
    data.controls = button;
    customUi(alg, data);
}

static void test_ui_submenu_memory() {
    OttHost h = makeOtt();
    auto* a = (_ottAlgorithm*)h.alg;

    if (pages[0].group == 0 ||
        pages[0].group != pages[1].group ||
        pages[1].group != pages[2].group)
        fail("band parameter pages do not preserve their selected row");
    if (pages[3].group == pages[0].group || pages[4].group == pages[0].group)
        fail("non-band parameter pages share the band selection group");

    // Low band: Threshold uses Up, Ratio remains Down, and Gain uses Pre.
    // Cycling views must restore each view's independent selection.
    pressUiButton(h.alg, kNT_potButtonL);
    if (!a->potUpper[UIState::THRESH][0])
        fail("threshold submenu did not select Up");

    pressUiButton(h.alg, kNT_button4); // Threshold -> Ratio
    if (a->potUpper[UIState::RATIO][0])
        fail("ratio submenu inherited threshold selection");

    pressUiButton(h.alg, kNT_button4); // Ratio -> Gain
    pressUiButton(h.alg, kNT_potButtonL);
    if (!a->potUpper[UIState::GAIN][0])
        fail("gain submenu did not select Pre");

    pressUiButton(h.alg, kNT_button4); // Gain -> Threshold
    if (!a->potUpper[UIState::THRESH][0])
        fail("threshold submenu selection was not remembered");

    pressUiButton(h.alg, kNT_button4); // Threshold -> Ratio
    if (a->potUpper[UIState::RATIO][0])
        fail("ratio submenu selection was not remembered");

    pressUiButton(h.alg, kNT_button4); // Ratio -> Gain
    if (!a->potUpper[UIState::GAIN][0])
        fail("gain submenu selection was not remembered");

    // Re-entering the display must retain view/submenu state and initialise
    // soft takeover from the selected parameter, not from an arbitrary centre.
    _NT_float3 pots = {};
    setupUi(h.alg, pots);
    if (a->state.potMode != UIState::GAIN ||
        !a->potUpper[UIState::GAIN][0])
        fail("setupUi reset the current submenu");
    const float expected =
        (h.v[kLoPreGain] - params[kLoPreGain].min) /
        float(params[kLoPreGain].max - params[kLoPreGain].min);
    if (fabsf(pots[0] - expected) > 1.0e-6f)
        fail("setupUi did not restore the selected parameter value");

    // Non-pot callbacks must never push stale pot positions into parameters.
    for (int p = 0; p < 3; ++p)
        a->potCaught[p] = true;
    gParameterPushes = 0;
    pressUiButton(h.alg, kNT_button3);
    if (gParameterPushes != 0)
        fail("button callback rewrote an untouched pot parameter");
}

static void test_ui_draw_narrow_threshold_regions() {
    OttHost h = makeOtt();
    // These extremes leave only one display pixel between a threshold and its
    // boundary. The ratio-line renderer must not compute a zero step count.
    for (int parameter : {kLoDownThr, kMidDownThr, kHiDownThr})
        h.v[parameter] = -10;
    for (int parameter : {kLoUpThr, kMidUpThr, kHiUpThr})
        h.v[parameter] = -590;
    if (!draw(h.alg))
        fail("custom UI did not draw narrow threshold regions");
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== OTT host tests ===\n";
    test_depth_zero_reconstruction();
    test_crossover_reconstruction();
    test_default_level();
    test_levels_across_amplitudes();
    test_band_gains();
    test_xfer_reference_transfer();
    test_detector_timebase_and_block_size();
    test_gain_math_accuracy();
    test_xfer_dynamic_reference();
    test_silence_does_not_charge_upward_gain();
    test_depth_law_and_upward_bound();
    test_xfer_static_law_calibration();
    test_attack_controls_gain_motion();
    test_mono_keeps_right_state_current();
    test_mono_input_fallback_keeps_stereo_outputs_equal();
    test_bypass_routing();
    test_stereo_dynamics_are_linked();
    test_ui_submenu_memory();
    test_ui_draw_narrow_threshold_regions();
    std::cout << "\nAll tests passed.\n";
    return 0;
}
