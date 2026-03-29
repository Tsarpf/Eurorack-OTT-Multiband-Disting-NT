# Vocoder Fixtures

This directory contains host-side test assets for the vocoder implementation.

Layout:

- `input/`: generated carrier/modulator WAV pairs
- `output/`: rendered vocoder outputs for each fixture
- `analysis/`: text and CSV reports from host rendering, benchmarking, and calibration

Generation flow:

1. build and run `generate_fixtures.cpp`
2. build and run `render_fixtures.cpp`
3. optionally run `calibrate_bandwidth.cpp`
4. optionally run `benchmark_vocoder.cpp`
