# OTT quality audit — 2026-09-05

This is the pre-fix audit. See [the implemented correction](FIX.md) for the subsequent Xfer-only work and current results.

Audited production source: commit `5d35b30` (OTT DSP last changed in `7cb4d8a`). No production DSP, defaults, plugin binary or device preset was changed during this audit.

## Conclusion

The current implementation closely reproduces the measured **settled Xfer OTT 1.37 transfer curves**, but substantially misses its **transient behavior and low-frequency nonlinear texture**. Matching static thresholds more closely is unlikely to address the main audible differences. Ableton's own OTT was not measured in this audit; these results establish fidelity to Xfer only.

## Method and evidence

- Compiled and ran current `make test`: all tests passed. Current maximum error on its 17 embedded reference points is 0.598 dB. An older `/tmp/test_ott_output.txt` log reported 1.64 dB; that log does not describe the current code.
- Compiled the actual production `ott_algo.cpp` and CMSIS float filters through the existing host test harness, then rendered interleaved float input at 48 kHz, 32-frame callbacks, dual-mono stereo, factory defaults unless Depth was explicitly varied. Unity in these offline files defines the reference amplitude; this is not a claim about NT analogue full scale.
- Recreated all 204 original steady-state probes: 40/500/8000 Hz, 17 levels from -120 to 0 dBFS peak, Depth 0/25/50/100, two seconds per plateau, last 0.5 seconds measured. Compared with archived Xfer measurements/audio in `/tmp/faust-ott-reference/xfer_ott_137_v3.{json,npz}`.
- Reran Xfer using Windows DawDreamer 0.9.0 with the existing `/tmp/faust-ott-xfer/OTT_x64.dll`, 48 kHz and 64-frame host blocks. Factory Time=100%, Depth=100%, gains=0 dB, Clean XOV=OFF, upward/downward strength=100%, branches active. Fresh 500 Hz step output is sample-identical to the archived timing output.
- Added fresh 40 and 8000 Hz step probes, a 500 Hz level transition spread over 10 ms, and eight synthetic 110 Hz harmonic plucks (20 harmonics, 2 ms onset, 120 ms decay, peak input 0.35).
- Working audit renderer, scripts, temporary experimental source copies, full audio and plugin-parameter dump are in `/tmp/ott-quality-20260905/`. That directory is temporary; only the numerical summaries and this report are preserved here. No plugin DLL or Python dependency files were added to the repository.

## Results

### Static transfer and crossover

| Measurement | Result |
|---|---:|
| All 204 steady points: RMS gain error | 0.190 dB |
| All 204 steady points: maximum gain error | 0.893 dB |
| Full Depth, 51 points: RMS gain error | 0.310 dB |
| Worst point | 40 Hz, -60 dBFS peak, Depth 100 |
| Stereo, L=-60 / R=0 dBFS, 500 Hz: native vs Xfer gain | -17.202 vs -17.338 dB |
| Stereo, L=-60 / R=-20 dBFS, 500 Hz: native vs Xfer gain | +1.083 vs +1.015 dB |

The Depth=0 summed impulse response is essentially flat: sampled magnitude differences versus Xfer are below 0.01 dB at 20/40/88/160/500/1000/2500/8000/16000/20000 Hz. Raw phase differences largely follow a two-sample additional delay in Xfer; they should not be misread as a large filter-shape defect. Small low-frequency residual phase differences remain. This validates the summed reconstruction, not identical individual-band behavior under unequal dynamic gains.

### Transients: the major mismatch

Each step alternates one-second plateaus at -50 and -8 dBFS peak. The first upward transition occurs at one second. Errors below compare output RMS over the first 10 ms after that transition.

| Probe | Native output above Xfer in first 10 ms | Native peak over whole probe | Xfer peak over whole probe |
|---|---:|---:|---:|
| 40 Hz step | 24.71 dB | 3.980 | 0.226 |
| 500 Hz step | 31.21 dB | 8.687 | 0.423 |
| 8000 Hz step | 26.18 dB | 9.870 | 0.564 |
| 500 Hz, 10 ms level ramp | 10.66 dB | 1.951 | 0.240 |
| Harmonic plucks | — | 3.063 | 0.354 |

Peak values are linear float amplitudes at equal input scaling, not level-matched listening scores. The harmonic-pluck output peak is about 18.7 dB above Xfer. These are synthetic diagnostic signals, not a music listening test.

At 500 Hz the mismatch lasts well beyond onset: +23.75 dB over 10–30 ms, +15.08 dB over 30–100 ms, +7.56 dB over 100–300 ms, and +2.31 dB over 300–800 ms. Settling eventually conceals this discrepancy in the static tests.

### Nonlinear texture

Fit and remove the fundamental from the last 0.5 seconds of a steady sine, then measure residual RMS relative to the fitted fundamental. This is distortion-plus-residual, not a claim that every remaining component is harmonic distortion.

| Input | Native residual | Xfer residual |
|---|---:|---:|
| 40 Hz, -20 dBFS peak | -57.34 dBc (~0.136%) | -29.29 dBc (~3.43%) |
| 500 Hz, -20 dBFS peak | -79.20 dBc (~0.011%) | -51.93 dBc (~0.253%) |

Our much smoother output is not necessarily a perceptual advantage. It shows that equal average gain does not reproduce Xfer's waveform shaping. Whether this texture is desirable, or matches Ableton, needs level-matched listening and an Ableton reference.

## Code diagnosis

1. **Stored upward gain survives arriving energy.** `ott_algo.cpp` computes `upMix = targetUp > up ? attackN : releaseN`, then smooths upward and downward *linear gain* independently and multiplies them. Falling upward gain uses the long release (132 ms high, 282 ms mid/low at defaults). Just before the 500 Hz step, the low/high bands each hold ~49.94 dB gain: 36 dB upward limit plus 13.94 dB band makeup. Their detectors see the quiet tone's small crossover leakage as active signal. The level change excites those bands while their gains are still large. At 10 ms the low band still has ~49.64 dB upward/reference gain. Even at 500 ms it retains ~38.52 dB. The existing true-silence guard does not prevent this scenario.
2. **A 5 ms power detector followed by gain attack/release is an unvalidated dynamic model.** Both its topology and the domain being smoothed matter. The current numerical attack/release defaults do not establish equivalent effective timing to Xfer. Separate branch/detector timing measurements are needed before assigning different constants.
3. **This is not primarily a callback-rate problem.** The usual 32-frame callback computes targets at 1.5 kHz and ramps gains per sample. Rerendering the same input with 4/16/48/64-frame blocks leaves the 500 Hz peak between 8.66 and 8.72, versus Xfer's 0.423. Expensive filter-coefficient generation does not occur at audio sample rate. No new NT CPU measurement was made.
4. **The existing tests miss the problem.** The reference comparison checks settled gain. The detector timebase test verifies its configured recurrence, not fidelity to measured Xfer dynamics. Add captured-reference transient envelopes and peak limits, including quiet-band excitation, before changing the DSP.
5. **Crossover motion remains unsmoothed.** Coefficients are replaced immediately while preserving DF2T state. Preserving state avoids a reset but does not guarantee click-free large frequency changes, despite the comment in `ott_structs.h`. This is a separate control-motion risk, not an explanation for poor factory-default sound; it was not experimentally swept here.

## Improvement order

1. Fix transient fidelity first: measure isolated upward/downward activation and recovery at several levels and frequencies, fit the detector/envelope topology and smoothing domain, and explicitly handle excessive stored upward gain when a quiet band receives energy. Preserve the already-good static transfer while doing so. A broadband limiter would mask this without matching the compressor's behavior.
2. Match low-frequency envelope ripple/nonlinear behavior, then evaluate level-matched musical material. Do not add arbitrary saturation merely to reproduce one residual percentage.
3. Validate input calibration using the actual NT signal levels and the intended reference level. The current transfer calibration uses numeric float amplitudes directly and does not establish an analogue-voltage mapping to DAW dBFS.
4. Measure Ableton's OTT preset through the existing Live Probe system before claiming parity with the user's preferred target. Current evidence is for Xfer only.
5. Refine crossover/control smoothing and any remaining sub-dB static differences after the large dynamic mismatch is resolved.

Cheap temporary experiments confirm this needs more than a cosmetic tweak: switching gain smoothing to dB reduced the first 10 ms 500 Hz mismatch from 31.21 to 30.73 dB; using the existing attack time to remove upward gain gave 30.52 dB; both together still gave 28.43 dB. These were diagnostic copies in `/tmp`, not production changes or recommended fixes.

The current implementation uses nine biquad stages per channel plus three band detectors. There is room to investigate a better envelope model without rebuilding the filterbank or moving everything to integer DSP. The actual CPU cost of a replacement must be benchmarked on the NT; no new hardware performance claim is made here.
