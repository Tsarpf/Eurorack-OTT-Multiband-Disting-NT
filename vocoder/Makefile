ROOT := ..
SDK ?= $(abspath $(ROOT)/distingnt_api)
PLUGIN := vocoder
OBJDIR := build

CMSIS_DSP := $(abspath $(ROOT)/CMSIS-DSP)
CMSIS_CORE := $(abspath $(ROOT)/CMSIS_6)

CXX := arm-none-eabi-g++
CXXFLAGS := -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fPIC \
	-I$(SDK)/include \
	-I$(CMSIS_DSP)/Include -I$(CMSIS_DSP)/PrivateInclude -I$(CMSIS_CORE)/CMSIS/Core/Include \
	-DARM_MATH_CM7 -D__FPU_PRESENT=1 \
	-O2 -ffast-math -fdata-sections -ffunction-sections \
	-fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables

CC := arm-none-eabi-gcc
CFLAGS := -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fPIC \
	-I$(CMSIS_DSP)/Include -I$(CMSIS_DSP)/PrivateInclude -I$(CMSIS_CORE)/CMSIS/Core/Include \
	-DARM_MATH_CM7 -D__FPU_PRESENT=1 \
	-O2 -ffast-math -fdata-sections -ffunction-sections

HOST_CXX ?= c++
HOST_CXXFLAGS ?= -std=c++17 -I$(SDK)/include

PYTHON ?= python
VENV_ACTIVATE := . $(abspath $(ROOT)/.venv-disting/bin/activate)
NTCTL := $(PYTHON) $(abspath $(ROOT)/tools/disting/ntctl.py)
PRESET ?= codex-dev
BENCH_SAMPLES ?= 8
BENCH_INTERVAL ?= 0.5

SRCS := vocoder_algo.cpp vocoder_ui.cpp
C_SRCS := $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_f32.c \
          $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_init_f32.c

CXX_OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SRCS))
C_OBJS := $(patsubst $(CMSIS_DSP)/Source/FilteringFunctions/%.c,$(OBJDIR)/cmsis_%.o,$(C_SRCS))

OBJS := $(CXX_OBJS) $(C_OBJS)

.PHONY: all build push test benchmark-host benchmark-device benchmark-device-motion version paths list clean

all: build

build: $(PLUGIN).o

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/cmsis_%.o: $(CMSIS_DSP)/Source/FilteringFunctions/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(PLUGIN).o: $(OBJS)
	$(CXX) -r $^ -o $@

push: build
	$(VENV_ACTIVATE) && $(NTCTL) push-plugin $(PLUGIN).o --save-as $(PRESET)

test:
	$(HOST_CXX) $(HOST_CXXFLAGS) test_vocoder.cpp -o /tmp/test_$(PLUGIN) && /tmp/test_$(PLUGIN)

benchmark-host:
	$(HOST_CXX) -O3 $(HOST_CXXFLAGS) benchmark_vocoder.cpp -o /tmp/benchmark_$(PLUGIN) && /tmp/benchmark_$(PLUGIN)

benchmark-device:
	$(VENV_ACTIVATE) && $(NTCTL) benchmark-vocoder --samples $(BENCH_SAMPLES) --interval $(BENCH_INTERVAL)

benchmark-device-motion:
	$(VENV_ACTIVATE) && $(NTCTL) benchmark-vocoder-motion --samples $(BENCH_SAMPLES) --interval $(BENCH_INTERVAL)

version:
	$(VENV_ACTIVATE) && $(NTCTL) version

paths:
	$(VENV_ACTIVATE) && $(NTCTL) paths

list:
	$(VENV_ACTIVATE) && $(NTCTL) list /programs/plug-ins

clean:
	rm -rf $(OBJDIR) $(PLUGIN).o
