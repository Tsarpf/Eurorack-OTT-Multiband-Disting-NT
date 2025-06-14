# ───────────────────────── paths ────────────────────────────────
SDK      ?= $(CURDIR)/distingnt_api
FAUST_BIN    ?= faust
FAUST_INC = $(CURDIR)/faust/architecture

# ───────────────────── plug-in naming  ───────────────────────────
PLUGIN   := ott

# We only *compile* the wrapper; ott_dsp.cpp is generated and #included
SRCS     := ott_wrapper.cpp
OBJS     := $(SRCS:.cpp=.o)

CXX      := arm-none-eabi-g++

# Compile flags (same as before, just factorised for readability)
CXXFLAGS := -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
            -mthumb -fPIC -I$(SDK)/include -I$(FAUST_INC) \
            -Os -ffast-math -fdata-sections -ffunction-sections \
            -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables

# ───────────────────── default target ────────────────────────────
all: $(PLUGIN).o

# 1. Re-generate the inline Faust code whenever ott.dsp changes
#    -i  → header-style, meant to be #included
ott_dsp.cpp: ott.dsp
	$(FAUST_BIN) -i -cn FaustDsp $< -o $@

# 2. Build the wrapper (depends on the generated file)
#    The wrapper itself #includes "ott_dsp.cpp", so the DSP code
#    gets compiled exactly once – inside this TU.
ott_wrapper.o: ott_wrapper.cpp ott_dsp.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 3. Link objects into a relocatable ELF that disting loads
$(PLUGIN).o: $(OBJS)
	$(CXX) -r $^ -o $@

clean:
	rm -f ott_dsp.cpp $(OBJS) $(PLUGIN).o
