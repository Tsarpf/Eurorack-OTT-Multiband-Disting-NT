
# Faust OTT Disting Plugin

Demo: https://www.youtube.com/watch?v=6zNhZXQEgkU

Demo jam: https://soundcloud.com/tsarpf/disting-nt-ott-style-compressor-test


This project builds a disting NT plug-in that uses DSP generated from the
[Faust](https://github.com/grame-cncm/faust) compiler.

The Faust source (`ott.dsp`) is translated to C++ during the build and linked
with custom C++ code split across `ott_algo.cpp`, `ott_ui.cpp` and `newlib_stub.cpp`.

## Using the plug-in
Make sure you have version 1.9 or higher of Disting NT, then just dl `ott.o` from this repo and copy it to `/programs/plug-ins/ott.o` on the Disting SD card, it will show up last in the algorithm list when you go to add a new one.

## Building it yourself

The build relies on Faust headers provided as a git submodule. Clone this
repository with submodules enabled:

```bash
git clone --recursive <repo-url>
```

If you have already cloned without `--recursive`, initialise the submodule with:

```bash
git submodule update --init --recursive
```

#### Building

The Makefile expects the ARM cross compiler (`arm-none-eabi-g++`) and the Faust
compiler (`faust`) to be in your PATH. Run:

```bash
make clean && make
```

This will generate `ott_dsp.cpp` from `ott.dsp` and build `ott.o`, which can be
loaded on a disting NT device.

## Environment and limitations

This plug-in targets the [disting NT](https://www.expert-sleepers.co.uk/) hardware. The Makefile builds `ott.o` using partial linking (`-r`) and disables exceptions and RTTI. The firmware does not provide standard C or C++ libraries. Instead `newlib_stub.cpp` implements a tiny bump allocator and minimal runtime support. Only lightweight headers like `<cstdint>` are used; containers and other STL facilities are unavailable. See `distingnt_api/include/distingnt/api.h` for the full API.

# License
Everything written by me is MIT licensed, check other code for their respective licenses
