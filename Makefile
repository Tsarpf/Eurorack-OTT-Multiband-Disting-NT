# ── paths ──────────────────────────────────────────────────────────────────────
SDK       ?= $(CURDIR)/distingnt_api
CMSIS_DSP ?= $(CURDIR)/CMSIS-DSP
CMSIS_CORE?= $(CURDIR)/CMSIS_6

PLUGIN := ott
OBJDIR := build

VENV_ACTIVATE := . $(abspath .venv-disting/bin/activate)
PYTHON        ?= python
NTCTL          = $(PYTHON) $(abspath tools/disting/ntctl.py)
PRESET        ?= codex-dev

HOST_CXX      ?= c++
HOST_CC       ?= cc
HOST_CXXFLAGS ?= -std=c++17 -I$(SDK)/include \
	-I$(CMSIS_DSP)/Include -I$(CMSIS_DSP)/PrivateInclude \
	-D__GNUC_PYTHON__
HOST_CFLAGS   ?= -I$(CMSIS_DSP)/Include -I$(CMSIS_DSP)/PrivateInclude \
	-D__GNUC_PYTHON__

# ── sources ────────────────────────────────────────────────────────────────────
CXX_SRCS := ott_algo.cpp ott_ui.cpp

C_SRCS := \
    $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_f32.c \
    $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_init_f32.c

CXX_OBJS := $(patsubst %.cpp,$(OBJDIR)/%.o,$(CXX_SRCS))
C_OBJS   := $(patsubst $(CMSIS_DSP)/Source/FilteringFunctions/%.c,$(OBJDIR)/cmsis_%.o,$(C_SRCS))
OBJS     := $(CXX_OBJS) $(C_OBJS)
HOST_CMSIS_OBJS := $(OBJDIR)/host_cmsis_df2t.o $(OBJDIR)/host_cmsis_df2t_init.o

# ── ARM cross-compiler ─────────────────────────────────────────────────────────
CXX := arm-none-eabi-g++
CC  := arm-none-eabi-gcc

CXXFLAGS := \
    -std=c++11 \
    -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fPIC \
    -I$(SDK)/include \
    -I$(CMSIS_DSP)/Include -I$(CMSIS_DSP)/PrivateInclude \
    -I$(CMSIS_CORE)/CMSIS/Core/Include \
    -DARM_MATH_CM7 -D__FPU_PRESENT=1 \
    -O2 -ffast-math -fdata-sections -ffunction-sections \
    -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables

CFLAGS := \
    -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fPIC \
    -I$(CMSIS_DSP)/Include -I$(CMSIS_DSP)/PrivateInclude \
    -I$(CMSIS_CORE)/CMSIS/Core/Include \
    -DARM_MATH_CM7 -D__FPU_PRESENT=1 \
    -O2 -ffast-math -fdata-sections -ffunction-sections

# ── build rules ────────────────────────────────────────────────────────────────
.PHONY: all build test test-matrix matrix push push-matrix clean

all: build

build: $(PLUGIN).o

matrix: matrix_mixer.o

matrix_mixer.o: matrix_mixer.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/cmsis_%.o: $(CMSIS_DSP)/Source/FilteringFunctions/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/host_cmsis_df2t.o: $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_f32.c
	@mkdir -p $(OBJDIR)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(OBJDIR)/host_cmsis_df2t_init.o: $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_init_f32.c
	@mkdir -p $(OBJDIR)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(PLUGIN).o: $(OBJS)
	$(CXX) -r $^ -o $@

test: $(HOST_CMSIS_OBJS)
	$(HOST_CXX) -O2 $(HOST_CXXFLAGS) test_ott.cpp $(HOST_CMSIS_OBJS) -o /tmp/test_$(PLUGIN) && /tmp/test_$(PLUGIN)

test-matrix:
	$(HOST_CXX) -O2 -std=c++17 -I$(SDK)/include test_matrix_mixer.cpp -o /tmp/test_matrix_mixer && /tmp/test_matrix_mixer

push: build
	$(VENV_ACTIVATE) && $(NTCTL) push-plugin $(PLUGIN).o --save-as $(PRESET)

push-matrix: matrix
	$(VENV_ACTIVATE) && $(NTCTL) push-plugin matrix_mixer.o --save-as $(PRESET)

clean:
	rm -rf $(OBJDIR) $(PLUGIN).o matrix_mixer.o
