# OTT Disting NT Plugin

Demo: https://www.youtube.com/watch?v=6zNhZXQEgkU

Demo jam: https://soundcloud.com/tsarpf/disting-nt-ott-style-compressor-test

This project builds a native C++ OTT-style multiband compressor plug-in for the
disting NT. The DSP implementation lives in `ott_algo.cpp` and `ott_dsp.h`, with
the custom interface in `ott_ui.cpp`. It uses CMSIS-DSP for the crossover filters.

## Using the plug-in

Make sure you have disting NT firmware 1.15 or newer, then copy `ott.o` from
this repository to `/programs/plug-ins/ott.o` on the disting SD card. It will
appear in the algorithm list after the plug-in is scanned and loaded.

## Building it yourself

Clone this repository with the disting NT API submodule enabled:

```bash
git clone --recursive <repo-url>
```

If you have already cloned without `--recursive`, initialise the submodule with:

```bash
git submodule update --init --recursive
```

### Building

The Makefile expects:

- the Arm GNU toolchain (`arm-none-eabi-g++` and `arm-none-eabi-gcc`) in `PATH`
- CMSIS-DSP in `CMSIS-DSP/`
- CMSIS Core in `CMSIS_6/`

The CMSIS locations can be overridden with the `CMSIS_DSP` and `CMSIS_CORE`
make variables. The versions used for this build can be fetched with:

```bash
git clone --depth 1 --branch v1.17.1 \
  https://github.com/ARM-software/CMSIS-DSP.git CMSIS-DSP
git clone --depth 1 --branch v6.3.0 \
  https://github.com/ARM-software/CMSIS_6.git CMSIS_6
```

Build and run the native host tests with:

```bash
make clean && make
make test
```

This builds `ott.o`, which can be loaded on a disting NT device.

## Environment and limitations

This plug-in targets the [disting NT](https://www.expert-sleepers.co.uk/)
hardware. The Makefile builds `ott.o` using partial linking (`-r`) and disables
exceptions and RTTI. All per-instance state is stored directly in the SRAM block
provided by the disting NT host; the plug-in does not install a custom allocator
or depend on a C++ heap. See `distingnt_api/include/distingnt/api.h` for the full
API.

## Xfer OTT reference renders

`tools/ott_reference.py` renders deterministic transfer curves, crossover
impulses, timing steps and control probes through Xfer OTT without opening its
editor or an audio device. It requires NumPy, DawDreamer and a native host
matching the plug-in binary (for example, Windows Python for `OTT_x64.dll`):

```bash
python tools/ott_reference.py /path/to/OTT_x64.dll \
  --output /tmp/xfer_ott_reference
```

The command writes numerical metadata to JSON and the source/reference audio to
a compressed NPZ file. Add `--extended` for the slower branch-isolation,
band-makeup, crossover and stereo-link probes used for detailed calibration.

## OTT dynamic reference validation

The OTT detector and two-sample lookahead are fitted to isolated Xfer dynamics.
Factory Attack is 10.0 ms in all bands; Release is 64.8 ms in low/mid and
30.3 ms in high. Existing presets retain saved values, so use these times or
create a fresh instance to get the measured default sound.

`make test` includes measured transient-envelope and peak regressions. For a
larger offline comparison, build `make build/ott_render`, then use
`tools/ott_dynamics.py capture`, `compare`, and `export-tests`. See the
[measurement and correction report](analysis/ott-quality-2026-09-05/FIX.md)
for commands, results, and remaining differences.

## Ableton Live reference renders

[Live Probe](tools/live_probe/README.md) runs batches through native Live effects
or hosted plug-ins using two generated Max for Live devices. The Python runner
plays generated probes or existing WAVs, sweeps parameters, captures stereo
float WAVs with parameter readbacks, and restores the initial settings. It
includes a Vocoder Modulator-mode width/depth sweep and a generic effect example.

The [native vocoder](vocoder/SPECS.md) uses a measured fourth-order filterbank
and sustained envelope contrast. Its
[before/after report](vocoder/fixtures/analysis/ableton_filterbank_update/README.md)
compares the current implementation with Live and the previous DSP.

## License

Original code in this repository is MIT licensed. Third-party dependencies keep
their respective licenses.
