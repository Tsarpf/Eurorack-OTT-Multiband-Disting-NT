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

Parameter indices and the reserved former Enhance slot are preserved. Legacy
Depth values above 200% clamp to 200%. Existing presets retain their stored
values, but sound different because the filter and envelope behavior changed.
Enhance is not implemented; the comparison measures it separately.

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
