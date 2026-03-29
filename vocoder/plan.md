# Disting NT Vocoder Port Plan

## Goal

Reimplement the verified Drambo vocoder behavior as a native Disting NT plug-in in pure C++.

The target behavior is:

- 40 parallel analysis/synthesis bands
- Log-spaced carrier/modulator band layout
- Two-region depth law
- Formant shift
- Enhance-style carrier normalization with optional high-frequency tilt
- Calibrated bandwidth compensation
- Custom Disting NT UI with meters and direct performance controls

This is a native Disting NT plug-in project, not a Faust project. Do not introduce Faust, generated DSP, or any custom memory manager.

## Non-Goals

- No Faust
- No custom allocator or per-instance heap tricks
- No speculative fixed-point rewrite in the first implementation
- No premature CMSIS-DSP rewrite before a measured CPU bottleneck exists
- No giant unrolled 40-band source copied from the Drambo DSL

## Source of Truth

The Drambo file is not portable source code, but it is the behavioral reference.

It defines the sound and control behavior we want to reproduce:

- band layout
- envelope behavior
- depth law
- formant behavior
- Enhance behavior
- synthesis Q floor
- bandwidth compensation concept

The implementation on Disting NT should match that behavior while being structured for embedded C++.

## Platform Constraints

The Disting NT target supports native C++ plug-ins, custom UI, and CPU usage reporting, but it is still an embedded system with finite CPU and memory budget.

Design assumptions:

- the audio hot loop must be simple and predictable
- transcendentals must stay out of the sample loop
- control smoothing is mandatory for moving filter parameters
- UI work must be decoupled from audio processing
- all per-instance state must live in the memory provided by the Disting NT API

## Top-Level Architecture

Split the implementation into three subsystems.

### 1. Bank Descriptor

This is the control-rate description of the current filter bank.

It should hold:

- active band count
- analysis center frequencies `f[i]`
- synthesis center frequencies `cf[i]`
- analysis coefficients per band
- synthesis coefficients per band
- current analysis Q
- current synthesis Q
- current bandwidth compensation gain
- per-band Enhance target tilt values

This layer is recomputed only when needed:

- when parameters change
- while smoothed parameters are still moving
- once per block during active smoothing

### 2. DSP State

This is the mutable per-instance state used in the hot loop.

Use structure-of-arrays layout, with contiguous float arrays.

Required state:

- `an_y1[40]`, `an_y2[40]`
- `sy_y1[40]`, `sy_y2[40]`
- `env[40]`
- `eAvg[40]`
- `cAvg[40]`
- shared carrier input history
- shared modulator input history
- decimated meter values for UI

This is the part worth putting in the fastest mutable memory available through the SDK.

### 3. Hot Loop

The hot loop should do only the minimum per sample:

- read current coefficients and state
- filter modulator band
- update asymmetric envelope follower
- update slow envelope average
- derive band gain from the two-region depth law
- filter carrier band
- optionally apply Enhance normalization
- sum active bands
- write the final output

The hot loop must not do:

- `pow`
- `sin`
- `cos`
- table interpolation
- allocation
- UI drawing
- control-page logic

The only meaningful branch inside the band loop should be logic required for:

- envelope attack vs release
- Enhance on vs off, if not already folded into precomputed values

The outer loop should stop at `activeBands`.

## Public Controls and UI Behavior

Main direct controls:

- left pot: Bandwidth
- center pot: Depth
- right pot: Formant
- left encoder: Band count, range 4 to 40
- right encoder: Dry/Wet

Additional parameters should include:

- Min Frequency
- Max Frequency
- Attack
- Release
- Enhance
- routing parameters required by the Disting NT API

UI goals:

- use OTT only as Disting NT UI inspiration and API usage reference
- do not reuse Faust-side patterns
- show per-band modulation or gain metering
- keep rendering simple and decimated
- do not read hot state at audio rate from UI code

## DSP Behavior Requirements

### Band Layout

- bands are log-spaced between current min and max frequency
- active band count changes the spacing of the whole bank
- the bank should be generated from the current active count, matching the Drambo behavior

### Analysis/Synthesis Filters

- use RBJ constant-peak bandpass form
- coefficient generation happens outside the sample loop
- analysis and synthesis filters use separate state

### Formant Shift

- synthesis frequencies are derived from analysis frequencies using a formant shift ratio
- formant movement must be smoothed to avoid zippering

### Bandwidth

- bandwidth affects analysis and synthesis Q behavior
- bandwidth movement must be smoothed
- synthesis Q floor must be preserved so formant movement stays audible

### Depth Law

Keep the two-region depth law:

- low-depth region blends toward unity
- middle point corresponds to roughly 1:1 envelope behavior
- upper region transitions into stronger peak-retention or exponential behavior

The law should match the Drambo behavior closely, but constants may be retuned after the C++ float32 port is audible and measurable.

### Enhance

Enhance is a real behavioral toggle, not a cosmetic parameter.

When enabled:

- normalize carrier band energy before modulation
- use a slow carrier-band average
- allow a mild HF tilt in the normalization target so the result brightens instead of flattening unnaturally

When disabled:

- bypass that normalization path cleanly

### Bandwidth Compensation

Bandwidth compensation must not remain a placeholder heuristic.

Implement:

- a measured 2D compensation table
- indexed by active band count and bandwidth
- generated offline
- interpolated at control rate, not sample rate

This compensation should reflect measured output behavior using a chosen reference source class.

## Required Implementation Order

### Phase 1: Clean Restart

Remove the bad DSP implementation and keep only reusable scaffolding.

Keep:

- Disting NT plug-in shell
- parameter/page definitions if still useful
- UI structure if useful
- build setup only if it is clean and native C++

Discard or rewrite:

- current DSP core
- placeholder calibration logic
- fake verification harness
- misleading generated fixtures

### Phase 1.5: TDD Harness Before DSP Work

Before writing the real vocoder engine, create host-side tests that define the intended behavior.

Required initial tests:

- descriptor generation
- band spacing sanity
- synthesis Q floor
- wet equals zero passthrough
- non-zero impulse response
- control motion stays finite
- Enhance changes output

The point is to lock down behavior before implementation, not after.

### Phase 2: 8-Band Float32 Prototype

Build the smallest musically useful version first.

Scope:

- 8 active bands
- descriptor/state/hot-loop split
- RBJ coefficient generator outside the hot loop
- working attack/release envelope follower
- slow envelope average
- two-region depth law
- formant shift
- Enhance behavior
- synthesis Q floor
- block-rate smoothing for Formant and Bandwidth

This phase is successful when the 8-band result sounds correct and stable.

### Phase 3: Verification Prototype

Before scaling to 40 bands, prove the architecture is correct.

Required checks:

- filter stability
- sensible output levels
- no zippering during moving controls
- no obvious clicks at block boundaries
- working Enhance toggle
- audible formant movement

This phase should still be host-driven first. Do not defer all validation to the hardware.

### Phase 4: Scale to 40 Bands

Once the 8-band version is trusted:

- increase arrays to 40 bands
- keep the same loop-driven design
- stop the band loop at `activeBands`
- add real 2D bandwidth compensation
- extend meter storage and UI rendering

Do not unroll the 40-band engine unless profiling later proves it is necessary.

### Phase 5: Disting NT UI and Profiling

Add the production UI and measure actual runtime cost.

Requirements:

- decimated per-band meters
- direct mapping of pots and encoders
- clean parameter pages
- on-device CPU validation

Profile with:

- UI active
- 40 bands
- intended sample rate
- intended block size

### Phase 6: Final Verification

Run the full verification suite and make final tuning adjustments.

At this stage the algorithm should be judged on:

- sound
- stability
- CPU budget
- control smoothness
- UI usefulness

## Verification Suite

The previous WAV-dumper style harness is not enough. A real verification suite should combine objective checks and listening fixtures.

Use at least these fixtures:

- impulse
- white or pink noise
- swept sine
- controls-under-motion sweep
- real neuro-bass or other musically relevant modulator/carrier case

What each fixture is for:

- impulse: confirm filter sanity and stability
- noise: measure bandwidth-related gain drift and build compensation
- swept sine: verify frequency placement, formant movement, and Q behavior
- moving controls: catch zippering, bad smoothing, and unstable interpolation
- neuro-bass: end-to-end subjective validation

Verification should include more than rendering files:

- level checks
- clipping checks
- discontinuity checks
- control-motion checks
- manual listening notes
- reference comparison against the Drambo behavior by ear and measurement where practical

## Host Benchmarking

Before on-device profiling, benchmark the float32 implementation on the development CPU.

Purpose:

- confirm scaling with band count
- compare static-control versus moving-control cost
- catch accidental regressions during refactors
- make sure the hot loop shape is sane before spending time on Disting deployment

Required host benchmark cases:

- 8 bands, static controls
- 8 bands, moving Formant and Bandwidth
- 16 bands, static and moving
- 24 bands, static and moving
- 32 bands, static and moving
- 40 bands, static and moving

Benchmark setup:

- 48 kHz
- representative block size, currently 24 frames
- at least several seconds of audio per run
- realistic carrier and modulator signals
- release build with host compiler optimization enabled

Benchmark output should report:

- elapsed milliseconds
- realtime multiplier
- normalized cost per frame

Host benchmark results do not replace Disting profiling, but they should be used as a gate before device deployment.

## Optimization Priorities

These are worth doing early.

### Do Early

- move all transcendentals and coefficient generation out of the sample loop
- use SoA state layout
- loop only over active bands
- smooth parameters at block rate instead of regenerating unstable coefficients per sample
- decimate metering

### Defer Until Profiling

- CMSIS-DSP integration
- fixed-point rewrite
- assembly-level tuning
- loop unrolling
- architecture-specific micro-optimizations

The rule is simple: get a correct float32 implementation first, then profile, then optimize only where measurements justify it.

## CPU Acceptance Target

Define the target up front:

- 40 bands
- intended sample rate, likely 48 kHz
- intended Disting NT block size
- custom UI active
- total CPU usage remains below a safe operating margin, roughly under 90%

If the float32 implementation already meets this target, stop optimizing.

## Development Workflow

Use the native Disting NT plug-in workflow:

1. Write or extend a host-side test first when behavior is being changed.
2. Implement the smallest change needed to satisfy that test.
3. Run the host verification suite.
4. Run the host benchmark suite.
5. Build object file.
6. Copy to `/programs/plug-ins`.
7. Trigger plug-in rescan or reload workflow.
8. Run verification patch or fixture on hardware.
9. Check sound and CPU usage.
10. Iterate.

The workflow should assume frequent redeploy and quick listening/profiling passes.

## File Strategy

Planned file responsibilities:

- `vocoder_algo.cpp`: algorithm entry points, descriptor updates, block processing
- `vocoder_dsp.h` or equivalent: coefficient helpers and lightweight DSP primitives
- `vocoder_structs.h`: descriptor and DSP state layout
- `vocoder_parameters.h`: parameters, pages, routing
- `vocoder_ui.cpp`: custom UI and decimated metering view
- `plan.md`: this implementation plan

If the current files contain broken assumptions, keep names if convenient but replace contents.

## Explicit Rules

- pure C++ only
- no Faust
- no custom memory manager
- one plug-in instance must not interfere with another
- no placeholder calibration shipped as final DSP
- no fake tests presented as verification
- no giant repeated-code translation from the DSL

## Definition of Done

The project is done when all of the following are true:

- the Disting NT plug-in is native C++
- the sound behavior matches the verified Drambo prototype closely
- 4 to 40 bands work correctly
- Bandwidth, Depth, Formant, band count, and dry/wet are mapped correctly to controls
- Enhance works as intended
- bandwidth compensation is measured and real
- UI is useful and stable
- parameter motion is smooth and click-free
- multiple instances work correctly
- CPU usage stays within the acceptance target

## Immediate Next Step

Start from a clean DSP restart:

1. keep only reusable Disting NT shell, parameter, and UI scaffolding
2. implement the 8-band descriptor/state/hot-loop prototype
3. make Formant and Bandwidth smoothing mandatory from day one
4. build a real verification harness before scaling to 40 bands
