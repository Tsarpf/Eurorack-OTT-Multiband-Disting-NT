# Vocoder Fixtures

This directory contains host-side test assets for the vocoder implementation.

Layout:

- `input/`: generated carrier/modulator WAV pairs
- `output/`: rendered vocoder outputs for each fixture
- `analysis/`: text and CSV reports from host rendering, benchmarking, and calibration

[Ableton comparison](analysis/ableton_comparison/README.md) contains measured
Depth, Width, Formant, and Release differences against Live's Vocoder in
40-band Modulator/Precise mode. Its reusable capture and analysis commands are
documented in [Live Probe](../../tools/live_probe/README.md#compare-the-native-vocoder-with-live).

[Filterbank update](analysis/ableton_filterbank_update/README.md) documents the
corrected filter responses, Depth and gain behavior, with measured before/after
results and remaining limitations.

Generation flow:

1. build and run `generate_fixtures.cpp`
2. build and run `render_fixtures.cpp`
3. optionally run `calibrate_bandwidth.cpp`
4. optionally run `benchmark_vocoder.cpp`
