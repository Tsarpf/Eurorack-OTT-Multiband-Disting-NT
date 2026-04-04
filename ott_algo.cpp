#include "ott_structs.h"
#include "ott_parameters.h"
#include <string.h>
#include <new>       // placement new

// ── Band index convention ─────────────────────────────────────────────────────
// 0 = Low, 1 = Mid, 2 = High  (matches draw order in ott_ui.cpp)

// ── Internal helpers ──────────────────────────────────────────────────────────

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
    }
}

// Recompute cached values for one band from the raw parameter array.
// band: 0=Low, 1=Mid, 2=High
// pXxx: parameter indices for this band's controls
static void recomputeBand(OttDSPState& d, const int16_t* v, int band,
                           int pDownThr, int pUpThr,
                           int pDownRat, int pUpRat,
                           int pPre,    int pPost,
                           int pRel)
{
    OttCached& c = d.cached;

    c.thrDown[band] = ottDbToLinear(v[pDownThr] * 0.1f);
    c.thrUp[band]   = ottDbToLinear(v[pUpThr]   * 0.1f);

    const float rDown = v[pDownRat] * 0.01f;   // param is %, so 100→1.0, 10000→100.0
    const float rUp   = v[pUpRat]   * 0.01f;
    c.exDown[band]  = 1.0f - 1.0f / (rDown > 1.0f ? rDown : 1.0f);
    c.exUp[band]    = 1.0f - 1.0f / (rUp   > 1.0f ? rUp   : 1.0f);

    c.preGain[band]  = ottDbToLinear(v[pPre]  * 0.1f);
    c.postGain[band] = ottDbToLinear(v[pPost] * 0.1f);

    const float relMs = v[pRel] * 0.1f;
    // per-sample decay coefficient; per-block version computed lazily in step()
    const float safeMs = relMs < 0.01f ? 0.01f : relMs;
    c.relCoeff[band] = expf(-1.0f / (safeMs * 0.001f * d.sr));
}

// Recompute everything from the full parameter array.
// Runs outside the audio thread; expf/powf/tanf are all fine here.
static void recomputeAll(_ottAlgorithm* a)
{
    OttDSPState& d = a->dsp;
    const int16_t* v = a->v;

    recomputeBand(d, v, 0,
                  kLoDownThr, kLoUpThr, kLoDownRat, kLoUpRat,
                  kLoPreGain, kLoPostGain, kLoRelease);

    recomputeBand(d, v, 1,
                  kMidDownThr, kMidUpThr, kMidDownRat, kMidUpRat,
                  kMidPreGain, kMidPostGain, kMidRelease);

    recomputeBand(d, v, 2,
                  kHiDownThr, kHiUpThr, kHiDownRat, kHiUpRat,
                  kHiPreGain, kHiPostGain, kHiRelease);

    d.cached.outGain    = ottDbToLinear(v[kGlobalOut] * 0.1f);
    d.cached.wet        = v[kGlobalWet] * 0.01f;
    // ~5 ms gain ramp — smooths zipper noise without adding noticeable lag
    d.cached.gainSmooth = expf(-1.0f / (0.005f * d.sr));

    recomputeXover(d, (float)v[kXoverLoMid], (float)v[kXoverMidHi]);

    // Force per-block release cache to recompute on next step()
    d.lastBlockN = 0;
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
    d.lastBlockN = 0;

    // Zero envelope state; gain states start at unity
    memset(&d.bands, 0, sizeof(d.bands));
    for (int ch = 0; ch < 2; ++ch)
        for (int b = 0; b < kOttBands; ++b)
            d.bands.gainState[ch][b] = 1.0f;

    // Safe initial compression state — nothing fires until parameterChanged
    // is called by the host for each parameter right after construct.
    for (int b = 0; b < kOttBands; ++b) {
        d.cached.thrDown[b]  = 2.0f;   // above 0 dBFS — downward never fires
        d.cached.thrUp[b]    = 0.0f;   // -inf — upward never fires
        d.cached.exDown[b]   = 0.75f;
        d.cached.exUp[b]     = 0.5f;
        d.cached.preGain[b]  = 1.0f;
        d.cached.postGain[b] = 1.0f;
        d.cached.relCoeff[b] = expf(-1.0f / (0.1f * d.sr));  // 100 ms
    }
    d.cached.gainSmooth = expf(-1.0f / (0.005f * d.sr));
    d.cached.outGain    = 1.0f;
    d.cached.wet        = 1.0f;

    // Crossover from params[] static defaults — safe, a->v not yet wired
    const float freqLoMid = (float)params[kXoverLoMid].def;
    const float freqMidHi = (float)params[kXoverMidHi].def;
    for (int ch = 0; ch < 2; ++ch) {
        ottComputeLR4(freqLoMid, d.sr, false, d.xover.lp1[ch].coeffs);
        ottComputeLR4(freqLoMid, d.sr, true,  d.xover.hp1[ch].coeffs);
        ottComputeLR4(freqMidHi, d.sr, false, d.xover.lp2[ch].coeffs);
        ottComputeLR4(freqMidHi, d.sr, true,  d.xover.hp2[ch].coeffs);
        ottLR4Init(d.xover.lp1[ch]);
        ottLR4Init(d.xover.hp1[ch]);
        ottLR4Init(d.xover.lp2[ch]);
        ottLR4Init(d.xover.hp2[ch]);
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

    // Skip recompute for pure routing params (indices 0–5)
    if (p >= kHiDownThr)
        recomputeAll(a);

    a->lastParam = p;
    a->lastValue = v;
}

// ── Audio step ────────────────────────────────────────────────────────────────

static void step(_NT_algorithm* s, float* bus, int nfBy4)
{
    auto* a = (_ottAlgorithm*)s;
    const int N  = nfBy4 * 4;
    const int Nc = N < kOttMaxBlock ? N : kOttMaxBlock;   // safety clamp

    float* inL  = bus + (s->v[kInL]  - 1) * N;
    float* inR  = bus + (s->v[kInR]  - 1) * N;
    float* outL = bus + (s->v[kOutL] - 1) * N;
    float* outR = bus + (s->v[kOutR] - 1) * N;
    const bool replL = (bool)s->v[kOutLMode];
    const bool replR = (bool)s->v[kOutRMode];

    const bool bypass = a->state.bypass || s->vIncludingCommon[0];
    if (bypass) {
        for (int i = 0; i < N; ++i) outL[i] = inL[i];
        for (int i = 0; i < N; ++i) outR[i] = inR[i];
        return;
    }

    OttDSPState& d = a->dsp;
    OttCached&   c = d.cached;

    // Lazily recompute per-block release coefficients when block size changes
    if (Nc != d.lastBlockN) {
        d.lastBlockN = Nc;
        for (int b = 0; b < kOttBands; ++b)
            d.relCoeffPerBlock[b] = powf(c.relCoeff[b], (float)Nc);
    }

    // Stack scratch buffers
    float dry[2][kOttMaxBlock];
    float rest[2][kOttMaxBlock];   // hp1 output (above f1), feeds lp2 + hp2
    float band[3][2][kOttMaxBlock];// [band][ch][sample]: low=0, mid=1, high=2

    // Preserve dry input before any writes (inL/outL may alias)
    memcpy(dry[0], inL, Nc * sizeof(float));
    memcpy(dry[1], inR, Nc * sizeof(float));

    // ── Crossover split ───────────────────────────────────────────────────────
    for (int ch = 0; ch < 2; ++ch) {
        const float* in = dry[ch];
        ottLR4Process(d.xover.lp1[ch], in,      band[0][ch], Nc);
        ottLR4Process(d.xover.hp1[ch], in,      rest[ch],    Nc);
        ottLR4Process(d.xover.lp2[ch], rest[ch], band[1][ch], Nc);
        ottLR4Process(d.xover.hp2[ch], rest[ch], band[2][ch], Nc);
    }

    // ── Per-band compression ──────────────────────────────────────────────────
    for (int b = 0; b < kOttBands; ++b) {
        const float relN = d.relCoeffPerBlock[b];
        const float pre  = c.preGain[b];
        const float post = c.postGain[b];
        const float gm   = c.gainSmooth;
        const float gmc  = 1.0f - gm;

        for (int ch = 0; ch < 2; ++ch) {
            float* buf = band[b][ch];

            // Pre-gain
            if (pre != 1.0f)
                for (int i = 0; i < Nc; ++i) buf[i] *= pre;

            // Block peak detection (instantaneous attack)
            float peak = 0.0f;
            for (int i = 0; i < Nc; ++i) {
                const float av = buf[i] < 0.0f ? -buf[i] : buf[i];
                if (av > peak) peak = av;
            }

            // Envelope: peak attack, smoothed release
            float& env = d.bands.env[ch][b];
            if (peak >= env) env = peak;
            else             env *= relN;

            // Gain computation (block rate, powf here is fine)
            const float tg = ottGain(env,
                                     c.thrDown[b], c.thrUp[b],
                                     c.exDown[b],  c.exUp[b]);

            // Apply gain with per-sample 1-pole smoother (anti-zipper)
            float& gs = d.bands.gainState[ch][b];
            for (int i = 0; i < Nc; ++i) {
                gs      = gm * gs + gmc * tg;
                buf[i] *= gs;
            }

            // Post-gain
            if (post != 1.0f)
                for (int i = 0; i < Nc; ++i) buf[i] *= post;
        }
    }

    // ── Sum bands + wet/dry blend + output gain → bus ─────────────────────────
    const float wet  = c.wet;
    const float dry1 = 1.0f - wet;
    const float og   = c.outGain;

    float* outs[2] = { outL, outR };
    const bool repls[2] = { replL, replR };

    for (int ch = 0; ch < 2; ++ch) {
        float*       out = outs[ch];
        const float* d0  = dry[ch];
        const float* b0  = band[0][ch];
        const float* b1  = band[1][ch];
        const float* b2  = band[2][ch];

        if (repls[ch]) {
            for (int i = 0; i < Nc; ++i)
                out[i] = (wet * (b0[i] + b1[i] + b2[i]) + dry1 * d0[i]) * og;
        } else {
            for (int i = 0; i < Nc; ++i)
                out[i] += (wet * (b0[i] + b1[i] + b2[i]) + dry1 * d0[i]) * og;
        }
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
