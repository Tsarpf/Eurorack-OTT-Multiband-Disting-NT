# Faust OTT Disting Plugin

This project builds a disting-NT plug-in that uses a DSP generated from the
[Faust](https://github.com/grame-cncm/faust) compiler.

The Faust source (`ott.dsp`) is translated to C++ during the build and linked
with a custom C++ UI in `ott_wrapper.cpp`.

## Cloning

The build relies on Faust headers provided as a git submodule. Clone this
repository with submodules enabled:

```bash
git clone --recursive <repo-url>
```

If you have already cloned without `--recursive`, initialise the submodule with:

```bash
git submodule update --init --recursive
```

## Building

The Makefile expects the ARM cross compiler (`arm-none-eabi-g++`) and the Faust
compiler (`faust`) to be in your PATH. Run:

```bash
make clean && make
```

This will generate `ott_dsp.cpp` from `ott.dsp` and build `ott.o`, which can be
loaded on a disting-NT device.
