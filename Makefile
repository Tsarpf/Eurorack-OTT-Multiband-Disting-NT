# ⇢  adjust these two paths if needed
SDK   ?= $(CURDIR)/distingnt_api
FAUST ?= faust

PLUGIN := ott

# ---------------------------------------------------------------------
#  Source files (hand-written)
SRCS := ott_algo.cpp \
        ott_ui.cpp   \
        newlib_stub.cpp

#  Faust-generated DSP
DSP_CPP := ott_dsp.cpp

#  Object files end up in build/ to keep the tree tidy
OBJDIR := build
OBJS   := $(patsubst %.cpp,$(OBJDIR)/%.o,$(notdir $(SRCS)))

CXX  := arm-none-eabi-g++
CXXFLAGS := -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fPIC \
            -I$(SDK)/include -I$(CURDIR)/faust/architecture \
            -Os -ffast-math -fdata-sections -ffunction-sections \
            -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables

# ---------------------------------------------------------------------
#  build rules
all: $(PLUGIN).o

#  ❶  generate the DSP
$(DSP_CPP): ott.dsp
	$(FAUST) -cn FaustDsp -mem -nvi -ct 1 -es 1 -mcd 16 -mdd 1024 -mdy 33 \
	         -single -ftz 0 $< -o $@

#  make sure the object directory exists
$(OBJDIR):
	@mkdir -p $(OBJDIR)

#  generic pattern rule for every .cpp
$(OBJDIR)/%.o: %.cpp | $(OBJDIR) $(DSP_CPP)
	$(CXX) $(CXXFLAGS) -c $< -o $@

#  link whole plug-in (partial link -r)
$(PLUGIN).o: $(OBJS)
	$(CXX) -r $^ -o $@

clean:
	rm -rf $(OBJDIR) $(DSP_CPP) $(PLUGIN).o

.PHONY: all clean
