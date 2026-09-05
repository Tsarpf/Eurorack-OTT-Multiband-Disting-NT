# Native vocoder DSP

The current filterbank is fitted against recordings of Ableton Live's Vocoder
in Modulator/Precise mode. See the
[measured implementation update](fixtures/analysis/ableton_filterbank_update/README.md)
and the [original comparison](fixtures/analysis/ableton_comparison/README.md).
This is an independently implemented approximation, not Ableton source code.

## Filterbank

Each analysis and synthesis band is a fourth-order Butterworth bandpass made
from two distinct second-order sections. Adjacent synthesis bands alternate
polarity. Center frequencies are geometrically spaced; Formant multiplies the
synthesis centers by `2^(semitones/12)`.

For log spacing `s = ln(maxHz/minHz)/(bands-1)` and fractional Width `w`, the
fitted prototype Q is `1.059133/(s*w)`. Analysis and synthesis use the same Q.
At 40 bands over 30 Hz–18 kHz this is approximately `6.457/w`. Digital bandwidth
compensation applies `sin(omega)/omega` before the bilinear transform. The fixed
gain after band summation is `0.13924/sqrt(w)`. Band-count scaling beyond the
40-band reference is an extrapolation that preserves relative overlap.

Width 10–200% follows the fit. The additional 0–10% range extends continuously
from a 5% equivalent width to 10%, avoiding infinite Q at zero. Narrow settings
still ring: matching narrow reference filters cannot eliminate their decay.

The runtime uses trapezoidal state-variable sections, following
[Andrew Simper's derivation](https://cytomic.com/files/dsp/SvfLinearTrapOptimised2.pdf).
These implement the same transfer function while retaining low-frequency
precision in float32. CMSIS DF2T remains as an independent reference path in
`batch_biquad.h`. Coefficients and state have persistent storage; control updates
never leave pointers to temporary coefficients. Synthesis coefficients are
smoothed during movement. A band-count change clears filter/envelope history
because band indices acquire new frequencies.

## Envelopes, level, and protection

Depth is a static function of each absolute band envelope against a **0.1362 V**
reference. Zero means unity band gain; 100% follows the envelope; higher values
increase sustained contrast and reject quiet bands. The fitted curve is in
`envelope_shape.h`. It contains no envelope/self-average normalization and no
automatic wet/dry makeup. It approximates measured level curves; it does not
identify Live's internal formula.

Attack uses the displayed millisecond value as the follower time constant.
Release uses one tenth of its displayed value, calibrated to the recorded
10/30/100 ms responses. An additional 1 ms interpolation smooths band gains.
The DC blocker is 5 Hz, below the minimum analysis band.

Input overload attenuation remains. Output attenuation starts at the existing
9 V soft-knee ceiling, followed by a bounded soft limiter approaching ±10 V.
This protection can prevent a match to very loud floating-point DAW outputs.
States below `1e-20` are flushed to zero to avoid denormal CPU costs.

## Controls and compatibility

| Control | Range | Default |
| --- | --- | --- |
| Bands | 4–40 | 16 |
| Width | 0–200% | 100% |
| Depth | 0–200% | 100% |
| Formant | −36–+36 semitones | 0 |
| Minimum frequency | 20–1000 Hz | 20 Hz |
| Maximum frequency | 2000–20000 Hz | 18000 Hz |
| Attack | 1–500 ms | 10 ms |
| Decay / Release | 1–1000 ms | 30 ms |
| Enhance | Off / On | On |

Parameter indices are preserved; the former reserved slot now exposes Enhance.
New instances default On. Existing presets keep their stored value (0 means Off).
Legacy Depth values above 200% clamp to 200%. The current device instance was
explicitly switched On while preserving the rest of its preset.

In the custom UI, button 1 toggles the disting NT's common bypass parameter,
matching OTT. The footer shows `BYPASS` while bypassed; holding the button does
not repeatedly toggle it.

## Build and validation

```sh
make -C vocoder build
make -C vocoder test
make -C vocoder render-probe
make -C vocoder benchmark-host
```

The target is the Cortex-M7 disting NT, using the SDK's supplied SRAM/DTC memory
and a maximum callback of 24 frames. The 48 kHz host renderer defaults to
±5 bus volts per ±1 WAV sample and divides output by the same scale. This is an
explicit comparison convention, not a calibration of a particular analog path;
`--bus-volts-per-full-scale 1` reproduces the original raw bus convention.

Tests cover routing and stereo state, sustained Depth behavior, release
recovery, precision at 20 Hz/high Q, extended control ranges, state ownership,
and extreme control motion. Host timings do not establish device CPU headroom;
the higher-order bank costs more than the old single-biquad design.


## Cortex-M7 optimization and device measurements

The production bank retains float TPT sections and fuses each two-section
cascade directly into its envelope peak or smoothed-gain output accumulation.
This removes intermediate buffer passes without changing filter history or
coefficient-update behavior. The connected disting NT measured **38.1%**
algorithm CPU static and **38.0%** during standard/full Width–Formant motion at
40 bands, versus **43.0%** before optimization (11.4–11.6% relative reduction).
Module averages were 43.0%, 44.3%, and 43.9%. The exact object is 20,872 bytes.

A faster mixed Q31 prototype measured 33.6%, but was rejected: shifted wide
filters near Nyquist exceeded its coefficient range; negative Formant exposed
low-frequency quantization tails; multiply-accumulates could wrap before final
saturation; and input scaling clipped large Pre transients. An analysis-only
variant with float synthesis and increased headroom still changed startup
behavior while Width coefficients moved. Matching stationary responses alone
was insufficient. Q15 also lacks resolution for narrow 20 Hz coefficients.
The shipped implementation therefore uses float DSP throughout.

Regression tests compare fused and buffered filter history, envelope peaks,
and synthesis output through live coefficient changes, 20–23,520 Hz centers,
Q up to 120, wide near-Nyquist filters, and 40 V input transients. They include
blocks above and below the NT's 24-frame callback size. Existing control,
routing, and high-Q tests remain in place.

See [device-performance.json](fixtures/analysis/ableton_filterbank_update/device-performance.json)
for object hashes, raw final captures, firmware, protocol, and rejected integer
experiments. Static runs use eight samples; motion runs use twelve samples,
six steps, and 0.05 s control settling. Both use 0.25 s intervals and one-second
settling. The temporary mono benchmark preset uses 20 Hz–18 kHz, Release 30 ms,
Depth 100%, and no injected signal. Standard motion sweeps Width 15–85% and
Formant ±18 semitones; full motion uses Width 0–200% and Formant ±36 semitones.
The corrected object remains installed; Matrix Mixer was restored afterward.


## Control update scheduling

Width/Formant changes no longer design the entire bank on every audio callback.
The algorithm snapshots a target and builds one analysis/synthesis band pair per
24 audio frames, then publishes the completed bank together. At 48 kHz, a
40-band bank completes in 20 ms (up to 50 Hz); four-frame callbacks retain the
same work rate. New knob changes queue the latest target without discarding
work already in progress. Running filter histories remain intact, and synthesis
coefficient interpolation continues at block rate. Final stationary filter
coefficients match the synchronous design.

The 120-step MIDI motion stress run, with zero explicit interval and zero
post-control settling delay, reported 38% algorithm CPU and at most 47% whole
module CPU. This exercises control changes but the MIDI transport and CPU meter
cannot measure every callback deadline. The scheduler regression separately
asserts the per-callback work budget. See
[control-update-fix.json](fixtures/analysis/ableton_filterbank_update/control-update-fix.json).

Depth above 100% remains an absolute-level expansion: the same sawtooth can get
quieter at low drive and louder at high drive. At 110 Hz, Width100, 40 bands,
raising Depth100→200 measured about -15 dB at 1 V peak, -5 dB at 2 V, and +1.6 dB
at 4 V. This release does not normalize or alter that gain curve. The custom
Formant display now uses plain integer/string printf conversions, avoiding sign
and width flags unsupported by some firmware formatters.


## Width transition and display follow-up

The abrupt bank publication regression is addressed by slewing the Width design
target with a 40 ms time constant, then interpolating both analysis and synthesis
coefficients and the bandwidth compensation over 20 ms. Both banks retain their
running state. Previously analysis jumped immediately while synthesis alone
interpolated; large Width steps also skipped the earlier target slew. The bounded
one-band-pair-per-24-frames coefficient design budget remains in place.

A 4 V peak 110 Hz saw at Depth100, 40 bands, 20–20k reproduced a Width200→100
transition minimum of 1.44 V RMS in 20 ms windows. The corrected transition
minimum is about 2.13 V RMS; the initial settled level is about 2.3 V RMS. A
regression requires that movement stay above 85% of the initial settled RMS,
with no change to the settled filter design or Depth curve.

The prior formatter changes were not validated against the actual older draw
calls. Formant and Gain now restore the `219b1fa` integer drawing exactly:
`snprintf(..., "%d", rawFormant)` and
`snprintf(..., "GAIN %d", rawGain)`. These show the stored integer/tenths values,
as that version did. The draw test checks the actual strings passed to
`NT_drawText`, including `-123` and `GAIN -60`, rather than just a host formatter.


Final device stress results for this follow-up: mono algorithm average 49.3%
(max50%), whole module max54%; stereo over20Hz–20kHz average86.5% (max87%),
whole module max91%. These are isolated-plugin motion tests, so additional
algorithms consume further CPU. Results and the exact object hash are in
[width-transition-fix.json](fixtures/analysis/ableton_filterbank_update/width-transition-fix.json).


## Enhance and CPU update — 2026-09-05

Enhance now normalizes each synthesis/carrier band's RMS before applying the
modulator's Depth gain. The measured approximation is 2:1 compression with a
maximum 11 dB boost: `sqrt(0.212132 / max(carrierRmsVolts, 0.0168503))`.
A 20 ms mean-square follower and 20 ms toggle transition precede the existing
1 ms gain interpolation. Quiet carrier bands receive more gain than strong
ones. This is an additional carrier process, independent of the modulator's
Attack/Release controls; it does not replace or normalize the Depth curve.

The 40-band Modulator/Precise captures use Release30 and the reference device's
actual 20 Hz–18 kHz range. Native processing still supports 20 kHz. At Width100,
Depth0, all 24 steady tones (40 Hz–16 kHz, -42/-18/-6 dBFS) match Enhance-on
output levels within 0.5 dB. White/pink broadband levels differ by less than
0.25 dB; individual spectral regions differ by up to about 2 dB. This remains
an approximation: Width50/200 and active Depth retain several-dB differences,
especially brown noise and quiet high bands. The 20 ms Enhance time constant
is a practical choice, not an identification of Live's exact transient law.

Enhance Off preserves the previous filterbank. Tested Depth0/100 renders are
bit-identical; other tested Depth/formant cases differ by at most 2.3e-7 full
scale, with residual RMS more than 100 dB below the signal. The bounded fast
power calculation is checked across Depth1–200 and a broad envelope range.

Performance changes reuse a filter pass where inputs and coefficients match
and filter-history residuals are below 1e-8 state units; retain coefficient
values across each sample loop; fuse carrier RMS measurement with filtering;
cache gain conversion; and avoid interpolating unused CMSIS reference arrays.
Coefficient convergence now uses a relative tolerance for large SVF values,
so float rounding cannot keep settled controls interpolating indefinitely.

Envelope and gain targets update after accumulating 24 samples (2 kHz at
48 kHz), including when the host delivers four-sample callbacks. Peak detection
and carrier power accumulate all samples. Audio filtering and gain interpolation
remain per sample. Coefficient construction retains the existing one-band-pair
per24frames budget, reaching 50 complete 40-band target banks per second.

In isolated NT tests, 40-band mono Depth100 averaged38% before, 30% with Enhance
Off, and32.2% with Enhance On. Stereo at those settings averaged75%,65.2%,72.2%
respectively. The final Enhance-on full-range stereo motion test reached90%
algorithm CPU and98% whole-module CPU: additional algorithms need headroom.
The current object was installed and all four original algorithms/parameters
were restored, with Enhance intentionally enabled on the existing vocoder.
See [measurements and limitations](fixtures/analysis/ableton_enhance_20260905/README.md).
