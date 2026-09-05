#include "ott_structs.h"
#include "ott_parameters.h"
#include <string.h>
#include <new>       // placement new

// ── Band index convention ─────────────────────────────────────────────────────
// 0 = Low, 1 = Mid, 2 = High  (matches draw order in ott_ui.cpp)

// ── Internal helpers ──────────────────────────────────────────────────────────

// Settled Xfer OTT 1.37 renders with both dynamics branches bypassed reveal a
// Depth-dependent per-band reference gain. It is separate from the visible
// Pre/Post controls (which remain unity by default).
static const float kReferenceGainFull[kOttBands] = {
    4.974981f, 2.929815f, 4.974981f // +13.9358, +9.3368, +13.9358 dB
};
// Xfer interpolates the fixed band lift in linear gain. Low/High share one
// Depth curve; Mid uses the slightly gentler curve measured from its bypassed
// reference path. The quadratic coefficient is (1 - linear coefficient).
static const float kReferenceDepthLinear[kOttBands] = {
    0.702699f, 0.750127f, 0.702699f
};
// Isolated branch fits with power averaging, peak decay and two samples of
// lookahead; see analysis/ott-quality-2026-09-05/FIX.md.
static const float kReferenceDetectorGainDb[kOttBands] = {4.979f, 5.147f, 5.245f};

// Align the dormant/duplicated right channel without copying filter instances:
// their internal pointers must continue to refer to their own state arrays.
static void mirrorRightChannelState(OttDSPState& d)
{
    memcpy(d.xover.lp1[1].state, d.xover.lp1[0].state,
           sizeof(d.xover.lp1[0].state));
    memcpy(d.xover.hp1[1].state, d.xover.hp1[0].state,
           sizeof(d.xover.hp1[0].state));
    memcpy(d.xover.lp2[1].state, d.xover.lp2[0].state,
           sizeof(d.xover.lp2[0].state));
    memcpy(d.xover.hp2[1].state, d.xover.hp2[0].state,
           sizeof(d.xover.hp2[0].state));
    memcpy(d.xover.lowPhase2[1].state, d.xover.lowPhase2[0].state,
           sizeof(d.xover.lowPhase2[0].state));
    for (int band = 0; band < kOttBands; ++band) {
        memcpy(d.bandDelay[band][1], d.bandDelay[band][0],
               sizeof(d.bandDelay[band][0]));
        d.bands.downGain[1][band] = d.bands.downGain[0][band];
        d.bands.upGain[1][band] = d.bands.upGain[0][band];
    }
}

// Recompute LR4 crossover coefficients for both channels.
// Called with current crossover Hz values directly from v[].
static void recomputeXover(OttDSPState& d, float freqLoMid, float freqMidHi)
{
    for (int ch = 0; ch < 2; ++ch) {
        ottComputeLR4(freqLoMid, d.sr, false, d.xover.lp1[ch].coeffs);
        ottLR4Reseat(d.xover.lp1[ch]);
        ottComputeLR4(freqLoMid, d.sr, true,  d.xover.hp1[ch].coeffs);
        ottLR4Reseat(d.xover.hp1[ch]);
        ottComputeLR4(freqMidHi, d.sr, false, d.xover.lp2[ch].coeffs);
        ottLR4Reseat(d.xover.lp2[ch]);
        ottComputeLR4(freqMidHi, d.sr, true,  d.xover.hp2[ch].coeffs);
        ottLR4Reseat(d.xover.hp2[ch]);
        ottComputeLR4Allpass(freqMidHi, d.sr,
                             d.xover.lowPhase2[ch].coeffs);
        ottAllpassReseat(d.xover.lowPhase2[ch]);
    }
}

// Recompute cached values for one band from the raw parameter array.
// band: 0=Low, 1=Mid, 2=High
// pXxx: parameter indices for this band's controls
static void recomputeBand(OttDSPState& d, const int16_t* v, int band,
                           int pDownThr, int pUpThr,
                           int pDownRat, int pUpRat,
                           int pPre,    int pPost,
                           int pAttack, int pRelease)
{
    OttCached& c = d.cached;

    c.thrDownDb[band] = v[pDownThr] * 0.1f;
    c.thrUpDb[band]   = v[pUpThr]   * 0.1f;

    // Ratios retain the plugin's original hundredths representation so saved
    // presets remain compatible. The int16 ceiling represents 327.67:1.
    const float rDown = v[pDownRat] * 0.01f;
    const float rUp   = v[pUpRat]   * 0.01f;
    c.exDown[band]  = 1.0f - 1.0f / (rDown > 1.0f ? rDown : 1.0f);
    c.exUp[band]    = 1.0f - 1.0f / (rUp   > 1.0f ? rUp   : 1.0f);

    c.preGainDb[band] = v[pPre] * 0.1f;
    c.preGain[band]  = ottDbToLinear(c.preGainDb[band]);
    c.postGain[band] = ottDbToLinear(v[pPost] * 0.1f);

    const float attackMs = v[pAttack] * 0.1f;
    const float releaseMs = v[pRelease] * 0.1f;
    const float safeAttack = attackMs < 0.01f ? 0.01f : attackMs;
    const float safeRelease = releaseMs < 0.01f ? 0.01f : releaseMs;
    c.attackCoeff[band] = expf(-1.0f / (safeAttack * 0.001f * d.sr));
    // Release is an amplitude time constant; the held state is power.
    c.releaseCoeff[band] = expf(-2.0f / (safeRelease * 0.001f * d.sr));
}

// Recompute everything from the full parameter array.
// Runs outside the audio thread; expf/powf/tanf are all fine here.
static void recomputeAll(_ottAlgorithm* a)
{
    OttDSPState& d = a->dsp;
    const int16_t* v = a->v;

    recomputeBand(d, v, 0,
                  kLoDownThr, kLoUpThr, kLoDownRat, kLoUpRat,
                  kLoPreGain, kLoPostGain, kLoAttack, kLoRelease);

    recomputeBand(d, v, 1,
                  kMidDownThr, kMidUpThr, kMidDownRat, kMidUpRat,
                  kMidPreGain, kMidPostGain, kMidAttack, kMidRelease);

    recomputeBand(d, v, 2,
                  kHiDownThr, kHiUpThr, kHiDownRat, kHiUpRat,
                  kHiPreGain, kHiPostGain, kHiAttack, kHiRelease);

    d.cached.outGain    = ottDbToLinear(v[kGlobalOut] * 0.1f);
    const float depth = v[kGlobalDepth] * 0.01f;
    // Dynamics slopes scale with raw Depth. Xfer also moves both static
    // thresholds slightly upward below full Depth; this quadratic goes through
    // the measured 25/50/100% positions while costing only two multiplies.
    d.cached.depth = depth;
    const float inverseDepth = 1.0f - depth;
    const float thresholdDeltaDb = inverseDepth *
        (2.908907f + 0.699627f * inverseDepth);
    for (int band = 0; band < kOttBands; ++band) {
        d.cached.thrDownDb[band] += thresholdDeltaDb;
        d.cached.thrUpDb[band] += thresholdDeltaDb;
        const float linear = kReferenceDepthLinear[band];
        const float referenceDepth = depth *
            (linear + (1.0f - linear) * depth);
        d.cached.referenceGain[band] =
            1.0f + referenceDepth * (kReferenceGainFull[band] - 1.0f);
    }
}

// ── Plugin entry points ───────────────────────────────────────────────────────

static void calculateRequirements(_NT_algorithmRequirements& r, const int32_t*)
{
    r.numParameters = kNumParams;
    r.sram = sizeof(_ottAlgorithm);
    r.dram = 0;
    r.dtc  = 0;
    r.itc  = 0;
}

static _NT_algorithm* construct(const _NT_algorithmMemoryPtrs& p,
                                const _NT_algorithmRequirements&, const int32_t*)
{
    auto* a = new (p.sram) _ottAlgorithm();
    a->parameters     = params;
    a->parameterPages = &paramPages;

    OttDSPState& d = a->dsp;
    d.sr         = (float)NT_globals.sampleRate;

    // Both directional gain states start at unity.
    for (int ch = 0; ch < 2; ++ch)
        for (int b = 0; b < kOttBands; ++b) {
            d.bands.downGain[ch][b] = 1.0f;
            d.bands.upGain[ch][b] = 1.0f;
        }

    // Safe initial compression state — nothing fires until parameterChanged
    // is called by the host for each parameter right after construct.
    for (int b = 0; b < kOttBands; ++b) {
        d.detectorPower[b] = 0.0f;
        d.cached.thrDownDb[b] = 6.0f;   // above 0 dBFS — downward never fires
        d.cached.thrUpDb[b]   = -120.0f;// effectively -inf
        d.cached.exDown[b]   = 0.75f;
        d.cached.exUp[b]     = 0.5f;
        d.cached.preGain[b]  = 1.0f;
        d.cached.preGainDb[b]= 0.0f;
        d.cached.postGain[b] = 1.0f;
        d.cached.attackCoeff[b] = expf(-1.0f / (0.01f * d.sr));
        d.cached.releaseCoeff[b] = expf(-2.0f / (0.1f * d.sr));
    }
    d.cached.outGain    = 1.0f;
    d.cached.depth      = 1.0f;
    for (int b = 0; b < kOttBands; ++b)
        d.cached.referenceGain[b] = kReferenceGainFull[b];

    // Crossover from params[] static defaults — safe, a->v not yet wired
    const float freqLoMid = (float)params[kXoverLoMid].def;
    const float freqMidHi = (float)params[kXoverMidHi].def;
    for (int ch = 0; ch < 2; ++ch) {
        ottComputeLR4(freqLoMid, d.sr, false, d.xover.lp1[ch].coeffs);
        ottComputeLR4(freqLoMid, d.sr, true,  d.xover.hp1[ch].coeffs);
        ottComputeLR4(freqMidHi, d.sr, false, d.xover.lp2[ch].coeffs);
        ottComputeLR4(freqMidHi, d.sr, true,  d.xover.hp2[ch].coeffs);
        ottComputeLR4Allpass(freqMidHi, d.sr,
                             d.xover.lowPhase2[ch].coeffs);
        ottLR4Init(d.xover.lp1[ch]);
        ottLR4Init(d.xover.hp1[ch]);
        ottLR4Init(d.xover.lp2[ch]);
        ottLR4Init(d.xover.hp2[ch]);
        ottAllpassInit(d.xover.lowPhase2[ch]);
    }

    return a;
}

static void parameterChanged(_NT_algorithm* s, int p)
{
    auto* a = (_ottAlgorithm*)s;
    const int16_t v = s->v[p];

    // Enforce crossover ordering: lo-mid < mid-hi
    switch (p) {
    case kXoverLoMid:
        if (v > s->v[kXoverMidHi]) { pushParam(s, kXoverLoMid, s->v[kXoverMidHi]); return; }
        break;
    case kXoverMidHi:
        if (v < s->v[kXoverLoMid]) { pushParam(s, kXoverMidHi, s->v[kXoverLoMid]); return; }
        break;
    // Enforce threshold ordering: down threshold >= up threshold
    case kHiDownThr:
        if (v < s->v[kHiUpThr])  { pushParam(s, kHiDownThr,  s->v[kHiUpThr]);  return; } break;
    case kHiUpThr:
        if (v > s->v[kHiDownThr]){ pushParam(s, kHiUpThr,    s->v[kHiDownThr]);return; } break;
    case kMidDownThr:
        if (v < s->v[kMidUpThr]) { pushParam(s, kMidDownThr, s->v[kMidUpThr]); return; } break;
    case kMidUpThr:
        if (v > s->v[kMidDownThr]){pushParam(s, kMidUpThr,   s->v[kMidDownThr]);return;} break;
    case kLoDownThr:
        if (v < s->v[kLoUpThr])  { pushParam(s, kLoDownThr,  s->v[kLoUpThr]);  return; } break;
    case kLoUpThr:
        if (v > s->v[kLoDownThr]){ pushParam(s, kLoUpThr,    s->v[kLoDownThr]);return; } break;
    default: break;
    }

    // Crossover coefficient work is independent of dynamics edits.
    if (p == kXoverLoMid || p == kXoverMidHi)
        recomputeXover(a->dsp, (float)s->v[kXoverLoMid],
                      (float)s->v[kXoverMidHi]);
    else if (p >= kHiDownThr)
        recomputeAll(a);

    a->lastParam = p;
    a->lastValue = v;
}

// ── Audio step ────────────────────────────────────────────────────────────────

static void step(_NT_algorithm* s, float* bus, int nfBy4)
{
    auto* a = (_ottAlgorithm*)s;
    const int N  = nfBy4 * 4;
    // disting NT's callback maximum is below this; keeping a single N avoids
    // silently leaving the tail of an unexpectedly large callback unwritten.
    if (N <= 0 || N > kOttMaxBlock)
        return;
    const int Nc = N;

    const bool stereo = s->v[kStereo] > 0;
    const int inBusL  = s->v[kIn];
    const bool stereoInput = stereo && inBusL < kNT_lastBus;
    const int inBusR  = stereoInput ? inBusL + 1 : inBusL;
    const int outBusL = s->v[kOut];
    const bool stereoOutput = stereo && outBusL < kNT_lastBus;
    const int outBusR = stereoOutput ? outBusL + 1 : outBusL;
    float* inL  = bus + (inBusL  - 1) * N;
    float* inR  = bus + (inBusR  - 1) * N;
    float* outL = bus + (outBusL - 1) * N;
    float* outR = bus + (outBusR - 1) * N;
    const bool repl   = (bool)s->v[kOutMode];
    const int numCh   = stereoOutput ? 2 : 1;

    if (s->vIncludingCommon[0]) {
        // Input and output buses may overlap (e.g. stereo In=1/2, Out=2/3),
        // so preserve both sources before either destination is touched.
        float bypassDry[2][kOttMaxBlock];
        memcpy(bypassDry[0], inL, N * sizeof(float));
        if (stereoOutput)
            memcpy(bypassDry[1], inR, N * sizeof(float));
        if (repl) {
            memcpy(outL, bypassDry[0], N * sizeof(float));
            if (stereoOutput)
                memcpy(outR, bypassDry[1], N * sizeof(float));
        } else {
            for (int i = 0; i < N; ++i)
                outL[i] += bypassDry[0][i];
            if (stereoOutput)
                for (int i = 0; i < N; ++i)
                    outR[i] += bypassDry[1][i];
        }
        return;
    }

    OttDSPState& d = a->dsp;
    OttCached&   c = d.cached;

    // If the selected input cannot provide a right-hand bus, both stereo
    // outputs represent the same mono source. Start them from identical
    // recursive state even after a preceding true-stereo interval.
    if (stereoOutput && !stereoInput)
        mirrorRightChannelState(d);

    // Stack scratch buffers
    float dry[2][kOttMaxBlock];
    float rest[2][kOttMaxBlock];   // hp1 output (above f1), feeds lp2 + hp2
    float band[3][2][kOttMaxBlock];// [band][ch][sample]: low=0, mid=1, high=2

    // Preserve dry input before any writes (inL/outL may alias)
    memcpy(dry[0], inL, Nc * sizeof(float));
    if (stereoOutput) memcpy(dry[1], inR, Nc * sizeof(float));

    // ── Crossover split ───────────────────────────────────────────────────────
    for (int ch = 0; ch < numCh; ++ch) {
        const float* in = dry[ch];
        ottLR4Process(d.xover.lp1[ch], in,      band[0][ch], Nc);
        ottAllpassProcess(d.xover.lowPhase2[ch], band[0][ch],
                          band[0][ch], Nc);
        ottLR4Process(d.xover.hp1[ch], in,      rest[ch],    Nc);
        ottLR4Process(d.xover.lp2[ch], rest[ch], band[1][ch], Nc);
        ottLR4Process(d.xover.hp2[ch], rest[ch], band[2][ch], Nc);
    }

    // Measured Xfer dynamics fit a short power average and a decaying peak.
    // Apply the curve directly to that envelope: smoothing gain independently
    // leaves large upward gain on an arriving transient. Two samples of audio
    // lookahead reproduce the measured crossover latency and transient peaks.
    for (int b = 0; b < kOttBands; ++b) {
        const float attack = c.attackCoeff[b];
        const float release = c.releaseCoeff[b];
        const float scale = c.referenceGain[b] * c.preGain[b] * c.postGain[b];
        float levelDb = -200.0f;
        bool activeSignal = false;
        for (int i = 0; i < Nc; ++i) {
            float power = band[b][0][i] * band[b][0][i];
            if (numCh == 2)
                power = 0.5f * (power + band[b][1][i] * band[b][1][i]);
            d.detectorPower[b] = attack * d.detectorPower[b] +
                                 (1.0f - attack) * power;
            d.envelopePower[b] = fmaxf(d.detectorPower[b],
                                      release * d.envelopePower[b]);
            activeSignal = d.envelopePower[b] > 1.0e-20f;
            if (!activeSignal) {
                d.detectorPower[b] = 0.0f;
                d.envelopePower[b] = 0.0f;
            }
            levelDb = 3.0102999566f * ottLog2(fmaxf(d.envelopePower[b], 1.0e-20f)) +
                      kReferenceDetectorGainDb[b] + c.preGainDb[b];
            const float gainDb = ottGainDb(levelDb, c.thrDownDb[b], c.thrUpDb[b],
                                           c.exDown[b], c.exUp[b], c.depth, activeSignal);
            const float gain = scale * ottExp2(gainDb * 0.1660964047f);
            for (int ch = 0; ch < numCh; ++ch) {
                // N is always a multiple of four, so each block starts on
                // the same phase of this two-sample delay.
                float& delayed = d.bandDelay[b][ch][i & 1];
                const float sample = delayed;
                delayed = band[b][ch][i];
                band[b][ch][i] = sample * gain;
            }
        }
        // Meter/debug state only: no expensive exponentials in the sample loop.
        ottGainTargets(levelDb, c.thrDownDb[b], c.thrUpDb[b],
                       c.exDown[b], c.exUp[b], c.depth, c.referenceGain[b],
                       activeSignal, d.bands.downGain[0][b], d.bands.upGain[0][b]);
        d.bands.downGain[1][b] = d.bands.downGain[0][b];
        d.bands.upGain[1][b] = d.bands.upGain[0][b];
    }

    // ── Sum bands + global output gain → bus ──────────────────────────────────
    // Depth has already scaled both compression curves.  Mixing raw dry with
    // the phase-rotated crossover reconstruction causes deep cancellation near
    // the crossover frequencies and is not equivalent to Xfer's Depth.
    const float og   = c.outGain;

    float* outs[2] = { outL, outR };

    for (int ch = 0; ch < numCh; ++ch) {
        float*       out = outs[ch];
        const float* b0  = band[0][ch];
        const float* b1  = band[1][ch];
        const float* b2  = band[2][ch];

        if (repl) {
            for (int i = 0; i < Nc; ++i)
                out[i] = (b0[i] + b1[i] + b2[i]) * og;
        } else {
            for (int i = 0; i < Nc; ++i)
                out[i] += (b0[i] + b1[i] + b2[i]) * og;
        }
    }

    if (!stereoOutput) {
        // Channel 1 is dormant in mono mode. Keep its recursive state aligned
        // with channel 0 so enabling stereo cannot reveal an old crossover tail
        // or a stale maximum upward gain on the first right-channel blocks.
        mirrorRightChannelState(d);
    }
}

// ── Factory / entry ───────────────────────────────────────────────────────────

static const _NT_factory factory = {
    .guid                = NT_MULTICHAR('O','T','T','1'),
    .name                = "OTT MB",
    .description         = "Multiband OTT compressor",
    .numSpecifications   = 0,
    .calculateRequirements = calculateRequirements,
    .construct           = construct,
    .parameterChanged    = parameterChanged,
    .step                = step,
    .draw                = draw,
    .tags                = kNT_tagUtility,
    .hasCustomUi         = hasCustomUi,
    .customUi            = customUi,
    .setupUi             = setupUi,
};

extern "C" uintptr_t pluginEntry(_NT_selector sel, uint32_t d)
{
    switch (sel) {
    case kNT_selector_version:      return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo:  return (uintptr_t)(d == 0 ? &factory : nullptr);
    }
    return 0;
}
