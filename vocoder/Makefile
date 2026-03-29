ROOT := ..
SDK ?= $(abspath $(ROOT)/distingnt_api)
PLUGIN := vocoder
OBJDIR := build

CXX := arm-none-eabi-g++
CXXFLAGS := -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fPIC \
	-I$(SDK)/include \
	-Os -ffast-math -fdata-sections -ffunction-sections \
	-fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables

HOST_CXX ?= c++
HOST_CXXFLAGS ?= -std=c++17 -I$(SDK)/include

PYTHON ?= python
VENV_ACTIVATE := . $(abspath $(ROOT)/.venv-disting/bin/activate)
NTCTL := $(PYTHON) $(abspath $(ROOT)/tools/disting/ntctl.py)
PRESET ?= codex-dev
BENCH_SAMPLES ?= 8
BENCH_INTERVAL ?= 0.5

SRCS := vocoder_algo.cpp vocoder_ui.cpp
OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(SRCS))

.PHONY: all build push test benchmark-host benchmark-device benchmark-device-motion version paths list clean

all: build

build: $(PLUGIN).o

$(OBJDIR)/.dir:
	mkdir -p $(OBJDIR)
	touch $@

$(OBJDIR)/%.o: %.cpp | $(OBJDIR)/.dir
	$(CXX) $(CXXFLAGS) -c $< -o $@

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
