# Measured OTT dynamics correction

Xfer OTT 1.37 is the sole reference for this change. The earlier audit's suggestion to measure Ableton is superseded by the user's decision to use Xfer.

## Identification before implementation

We captured all three bands separately, each with both branches bypassed, upward only, downward only, and both active. Dividing a branch render by its bypassed band render isolates its gain trajectory. Fits exclude plugin startup and ill-conditioned divisions near carrier zero crossings.

Several plausible detector models were fitted to these trajectories. An asymmetric rectified follower gave about 0.07–0.11 dB fitting error. A power average followed by an exponentially decaying peak was substantially closer. Accounting for the two-sample lookahead visible in the impulse response brought the mid-band fitting error to 0.00014 dB. These are black-box fits, not claims about Xfer's source code.

The fitted power-average time constants were 9.946/10.002/9.991 ms for low/mid/high. The peak amplitude decay constants were 64.722/64.776/30.317 ms. Production defaults use 10 ms attack in all bands and 64.8/64.8/30.3 ms release. Small per-band detector calibration offsets retain the static threshold/ratio calibration.

## Implementation

- Run the stereo-linked power average and decaying peak per sample.
- Apply the existing upward/downward static curves directly to that envelope. Do not independently smooth and multiply two lingering gain states.
- Delay band audio by two samples while the detector sees current input.
- Evaluate one combined gain exponent per sample with bounded float log2/exp2 approximations. They are checked against libm across the detector/gain range: log2 absolute error below 1e-5 and exp2 relative error below 1e-5. The sample loop makes no libm calls; coefficient math remains outside it and debug/meter gain conversion is once per band/block.
- Preserve stereo/routing state, including the new delay state. The complete audio path is checked across 4/16/32/48/64-frame callbacks.
- Add build dependencies so changing a shared struct/header rebuilds the UI and DSP together. This avoids incompatible object layouts in an incremental ARM build.

No crossover redesign, saturation or output limiter was needed. The low-frequency nonlinear texture emerged from the measured detector model itself.

## Results at factory defaults

| Measurement | Before | After | Xfer |
|---|---:|---:|---:|
| 204-point static gain RMS error | 0.190 dB | 0.034 dB | — |
| Worst static point error | 0.893 dB | 0.201 dB | — |
| 500 Hz rising step, first 10 ms output error | +31.21 dB | -0.038 dB | — |
| 40 Hz step output peak | 3.980 | 0.228 | 0.226 |
| 500 Hz step output peak | 8.687 | 0.421 | 0.423 |
| 8 kHz step output peak | 9.870 | 0.564 | 0.564 |
| 40 Hz / -20 dBFS residual beyond fundamental | -57.34 dBc | -29.31 dBc | -29.29 dBc |

Steady tests used the previous 204-point reference; dynamic validation also used newly generated captures. The three band-centre steps were used for fitting. Validation included 10 ms ramps and harmonic plucks; held-out tests added plucks at Depth 25/50, new 110 Hz levels, asymmetric stereo steps and deterministic broadband bursts. Full 10 ms envelope RMS errors are in `fixed-dynamics.json`; burst noise is 0.014 dB, and plucks at Depth 25/50/100 are 0.032/0.039/0.050 dB.

The remaining worst regression window is a 0.77 dB difference during the 10–30 ms recovery of the falling 500 Hz step. Tests allow 0.9 dB envelope error and 0.5 dB peak error on the six measured step/pluck cases. The previous implementation misses these by tens of dB. Static reference test tolerance was tightened from 2.1 to 0.4 dB.

## Limits and deployment

Xfer's instance-startup parameter ramps are not modeled. This explains the maximum 10 ms errors in the full-file metrics: they occur in the first window after creation. The asymmetric stereo probe starts with loud input immediately and has a 2.69 dB initial peak difference; after 100 ms, native/reference peaks are 0.306/0.299. This is separate from the original large overshoots on running quiet-to-loud signals.

All host tests pass, including ASan/UBSan, measured transient regression fixtures, and a host build with the firmware's fast-math option. The ARM plugin builds and loads successfully on the NT. After the user connected the device, both the old and new binaries were benchmarked in isolated presets, with output routed to internal auxiliary buses 27/28.

| NT algorithm CPU | Previous build, average / maximum | Fixed build, average / maximum |
|---|---:|---:|
| Mono | 3.0% / 3% | 7.0% / 7% |
| Stereo | 5.1% / 6% | 9.0% / 9% |
| Stereo with Depth/Attack/Release edits | 5.1% / 6% | 9.0% / 9% |

Whole-module maxima for the fixed build were 11%, 13%, and 14%, respectively. Each case used 12 CPU samples, physical input buses 1/2, and a fresh instance. Input audio was not controlled or amplitude-verified; these are CPU measurements, not a hardware audio comparison or a bound over every possible signal. The active control edits covered Depth 0/25/50/100, high Attack 0.1/10/500 ms and high Release 1/30.3/2000 ms.

The exact measured fixed binary (`ott.o`, SHA256 `6f64d58bf18d424f88cae6237eaf675750afd773077b3eeb5ba0517233d5f862`) remains installed at `/programs/plug-ins/ott.o`. The original `/presets/matrix_mixer.json` preset and every captured parameter were restored and verified equal. No OTT was added to that preset, which contains Tuner, Matrix Mixer and Vocoder. Raw CPU samples and firmware version are in `device-benchmark.json`.

Existing presets keep their saved times. To use the measured default sound, instantiate a fresh OTT or set all attacks to **10.0 ms**, low/mid release to **64.8 ms**, and high release to **30.3 ms**. Attack now sets the power-average time constant; Release sets the held peak's amplitude decay time constant. `ntctl.py`'s OTT default table was updated accordingly.

## Reproduction

Build the native renderer and run the embedded fixtures:

```sh
make test build/ott_render
```

On a platform matching the VST binary, with NumPy and DawDreamer installed:

```sh
python tools/ott_dynamics.py capture /path/to/OTT_x64.dll /path/to/reference.npz --branches
```

On Linux or Windows with NumPy and a native renderer:

```sh
python tools/ott_dynamics.py compare /path/to/reference.npz build/ott_render /path/to/comparison.json
python tools/ott_dynamics.py export-tests /path/to/reference.npz fixtures/ott_dynamics_reference.h
```

The committed fixture contains black-box measurements and the capture hash, so `make test` needs neither Python nor Xfer. `reference-metadata.json` records the plugin binary hash and exact controls. Full audio remains in `/tmp/ott-quality-20260905/dynamics-reference.npz` on the audit machine and can be regenerated with the capture command; the 20 MB audio archive and proprietary plugin are not committed.
