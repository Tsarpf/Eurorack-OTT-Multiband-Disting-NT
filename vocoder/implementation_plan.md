# Vocoder CPU Optimization Plan

## Problem

The 40-band vocoder is nearly maxing the Disting NT's ARM Cortex-M7 @ 600 MHz, leaving no headroom for other algorithms. Goal: reduce CPU **without sacrificing sound quality**.

## Root Cause Analysis

The [step()](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/vocoder_algo.cpp#318-789) function processes audio **sample-by-sample** with a per-band inner loop. Each biquad filter iteration has a tight data dependency chain (`y[n]` depends on `y[n-1]`, `y[n-2]`). On the Cortex-M7:

| Instruction | Throughput | **Latency** |
|---|---|---|
| Integer SMLAD (dual 16-bit MAC) | 1 cycle | 1–2 cycles |
| Float VMLA.F32 (float MAC) | 1 cycle | **6 cycles** |

**The FPU stalls 4–5 cycles per biquad multiply-accumulate** because each MAC depends on the previous result. At 40 bands × 2 channels × 48 kHz, this is the dominant bottleneck.

---

## Strategy: Two-Tier Approach

### Tier 1 — **CMSIS-DSP Batch Biquad Processing** (primary, architectural)

Replace the hand-written sample-by-sample biquad loops with CMSIS-DSP's `arm_biquad_cascade_df2T_f32` (Transposed Direct Form II), which:

1. **Processes N samples at once** — the batch API interleaves independent operations to hide the 6-cycle FPU latency
2. **Uses only 2 state vars per stage** (vs 4 in DF1) — half the memory, better cache/DTC utilization
3. **Is hand-optimized in assembly** by ARM for the Cortex-M7 pipeline
4. Requires restructuring from "for each sample → for each band" to **"for each band → process all N samples"**

#### Architecture Change

```
BEFORE (current):                      AFTER (CMSIS-DSP):
for sample in block:                   for band in 40:
  for band in 40:                        arm_biquad_cascade_df2T_f32(
    analysis_biquad(sample)                analysisInst[band],
    synthesis_biquad(sample)               modulatorBuf, analysisBuf,
    envelope_update(sample)                blockSize)
                                         arm_biquad_cascade_df2T_f32(
                                           synthesisInst[band],
                                           carrierBuf, synthesisBuf,
                                           blockSize)
                                         // batch envelope + mixing
```

> [!IMPORTANT]
> This is a significant architectural restructure of [step()](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/vocoder_algo.cpp#318-789). The envelope followers, gain smoothing, and cross-fade logic must also be refactored to work in a band-first-then-samples order. The work buffer (`NT_globals.workBuffer`) can be used for temporary per-band output buffers.

#### Build Integration

Add CMSIS-DSP to the Makefile:

```makefile
CMSIS_DSP ?= $(abspath $(ROOT)/CMSIS-DSP)
CXXFLAGS += -I$(CMSIS_DSP)/Include -DARM_MATH_CM7 -D__FPU_PRESENT=1
```

Either link precompiled `libarm_cortexM7lfsp_math.a` or compile the specific source files needed (`arm_biquad_cascade_df2T_f32.c` etc.) directly.

#### Expected Impact: **40–60% CPU reduction** on the filter bank (which is ~80% of total CPU)

---

### Tier 2 — Low-Hanging Micro-Optimizations (complement Tier 1)

These are additive wins on top of the architectural change:

| # | Optimization | Impact | Risk |
|---|---|---|---|
| 1 | Analysis decimation 2→4 | ~15–20% fewer analysis biquads | Very low |
| 2 | Band control interval 4→8 | ~8–10% fewer envelope updates | None |
| 3 | Fix coefficient smoothing inside ch loop (bug) | ~3–5% | None (correctness fix) |
| 4 | Cache [vocoderMixCoeffFromSeconds](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/vocoder_dsp.h#40-44) `expf` calls | ~2–3% | None |
| 5 | Compiler: `-O2` instead of `-Os` | ~5–15% | None |
| 6 | Gate synthesisBandGain smoothing when clean | ~2–3% | None |

---

### Integer (Q15/Q31) Verdict

> [!NOTE]
> On the Cortex-M7 with hardware FPU, **integer Q15/Q31 does NOT provide a clear speed advantage** over float32 for biquad processing. The FPU matches integer throughput (1 cycle). The real win comes from batch processing that hides the latency difference. Additionally, Q15 risks overflow distortion and requires complex scaling management that could harm sound quality. **Recommendation: stay with float32 + CMSIS-DSP batch processing.**

---

## Proposed Changes

### CMSIS-DSP Integration

#### [NEW] CMSIS-DSP dependency
- Download/clone ARM CMSIS-DSP into the project root
- Add required source files and includes to build

#### [MODIFY] [Makefile](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/Makefile)
- Add CMSIS-DSP include path and defines (`-DARM_MATH_CM7`)
- Change `-Os` to `-O2`
- Add CMSIS-DSP source files or prebuilt library link

#### [MODIFY] [vocoder_structs.h](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/vocoder_structs.h)
- Add `arm_biquad_casd_df2T_inst_f32` instances for analysis and synthesis filter banks
- Restructure state arrays for batch processing

#### [MODIFY] [vocoder_algo.cpp](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/vocoder_algo.cpp)
- Restructure [step()](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/vocoder_algo.cpp#318-789) from sample-first to band-first loop order
- Replace hand-written biquads with `arm_biquad_cascade_df2T_f32()` calls
- Refactor envelope/gain/metering to batch processing
- Fix coefficient smoothing bug (move outside channel loop)  
- Increase `analysisInterval` to 4, `bandControlInterval` to 8
- Cache `expf` coefficients in control state

---

## User Review Required

> [!WARNING]
> **Scope decision needed**: The CMSIS-DSP restructure is a large change to [step()](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/vocoder_algo.cpp#318-789). Two approaches:
> 
> **Option A — Full restructure**: Rewrite [step()](file:///Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder/vocoder_algo.cpp#318-789) to band-first with CMSIS-DSP batch biquads. Maximum performance gain (~40–60% on filters + tier 2 savings). Bigger diff, needs careful A/B testing.
> 
> **Option B — Incremental**: Apply tier 2 micro-optimizations first (smaller, safer changes, ~30–45% combined), then tackle CMSIS-DSP in a follow-up. Lower risk per step.

## Verification Plan

### Automated Tests
```bash
cd /Users/tesatesa/devaus/disting-nt-ott-and-vocoder/vocoder
make test              # functional correctness
make benchmark-host    # relative performance comparison
```

### Device Testing
```bash
make push && make benchmark-device && make benchmark-device-motion
```

### Manual
- A/B listening test on device with typical patches
- CPU meter comparison before/after
- Formant sweep artifact check
