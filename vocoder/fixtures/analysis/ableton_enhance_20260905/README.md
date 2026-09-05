# Enhance measurement and performance update

Reference: Ableton Vocoder, Modulator carrier, 40 bands, Precise, 20 Hz–18 kHz,
Attack10 ms, Release30 ms, wet100%, zero output gain, gate disabled. Live's
actual upper-frequency parameter stops at18 kHz. The NT supports20 kHz and
was benchmarked over20 Hz–20 kHz.

`comparison.json` records ten captures, reference/output hashes, alignment,
steady levels and spectral errors. Raw audio remains in the LiveProbe comparison
directory named in the report. `measure_enhance.py` regenerates deterministic
signals and automates both capture batches, restoring touched Live parameters.

```sh
make -C vocoder render-probe
python3 -m tools.live_probe.measure_enhance \
  --root /path/to/new/comparison --capture --bridge /path/to/LiveProbe/bridge
# Re-analyze existing captures without touching Live:
python3 -m tools.live_probe.measure_enhance --root /path/to/comparison
```

Depth0 isolates carrier processing. Live's tone level steps imply approximately
2:1 compression and a maximum11 dB boost. RMS band normalization matches both
tones and broadband noise more closely than peak normalization. The implemented
reference is0.212132 V RMS, with a20 ms mean-square follower and20 ms toggle
crossfade. Detector timing and the use of the synthesis bands are approximations.

At Width100/Depth0, the final Enhance-on tone errors are -0.50..+0.39 dB over
24 measurements. White/pink noise whole-signal errors are -0.20/+0.23 dB;
spectral errors reach roughly2 dB. Width50/200 retain around +/-1.5 dB carrier
level errors. With Depth100, measured broadband errors span about -4.7..+3.6 dB,
and some spectral regions differ by6–7 dB. These include the pre-existing
filterbank/envelope approximation. This update does not claim a null match to
Live, exact Enhance transients, or a complete new Formant calibration.

`off-regression.json` compares the previous renderer (commit46a1572) with
Enhance Off on a seeded noise/saw signal, Width0/50/100/200, Depth0/50/100/170,
and Formant0/±36. Depth0/100 cases are bit-identical. Other cases have at most
2.24e-7 absolute full-scale error, residual RMS below -101 dB relative to output.

`device-performance.json` contains the original and final isolated-plugin CPU
readings and the installed object hash. Preset restoration verified the original
four algorithms and all parameter values, except the explicitly enabled Enhance.
Output used auxiliary buses27/28. Inputs were physical, unrecorded and not
level-controlled, so small CPU differences between captures are not significant.
Ten readings per case,150 ms apart, cannot prove every callback deadline.

| 40 bands, self carrier, Formant0 | Previous | Enhance Off | Enhance On |
| --- | ---: | ---: | ---: |
| Mono, Depth100 | 38.0% | 30.0% | 32.2% |
| Stereo, Depth100 | 75.0% | 65.2% | 72.2% |
| Mono, Depth170 | 41.0% | 32.3% | — |
| Stereo, Depth170 | 81.0% | 70.3% | 67.4% |

Signal/history-dependent sharing makes the stereo numbers vary. The final
Enhance-on +12 semitone stereo case averaged81.4% (max82%). Full Width/Formant
motion at Depth170 averaged47.9% mono and90% stereo. Whole-module maxima were
53% and98%. Forty-band stereo movement still leaves very little CPU for other
algorithms; the sampled result is not a universal overload guarantee.

`performance-experiments.json` retains rejected/intermediate runs. In particular,
O3 was worse and some early Enhance builds reached100% or triggered deactivation.
The final build keeps O2. A loader failure from an unavailable `memcmp` import
was corrected with inline comparisons, then preset restoration was reverified.

Validation: native vocoder suite (including four-vs24-sample callbacks, exact
filter output/state comparisons, RMS accumulation, full-range Enhance motion,
coefficient convergence and numerical Depth error); 16 Live bridge unit tests;
ARM build/import inspection; ten actual Live recordings; NT benchmarks and
preset restoration. Button1 bypass and the original integer Formant/Gain draw
calls are preserved.
