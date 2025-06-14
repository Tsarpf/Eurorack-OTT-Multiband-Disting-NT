SDK   ?= $(CURDIR)/distingnt_api
FAUST ?= faust
PLUGIN := ott

# Build just the wrapper – it #includes ott_dsp.cpp
#SRCS := ott_wrapper.cpp
SRCS := ott_wrapper.cpp newlib_stub.cpp
OBJS := $(SRCS:.cpp=.o)
CXX  := arm-none-eabi-g++

all: $(PLUGIN).o

# ❶ Generate plain C++ DSP with custom memory manager support
ott_dsp.cpp: ott.dsp
	$(FAUST) -cn FaustDsp -mem -nvi -ct 1 -es 1 -mcd 16 -mdd 1024 -mdy 33 -single -ftz 0 $< -o $@

# ❷ Wrapper needs that file present when it’s compiled
ott_wrapper.o: ott_dsp.cpp

%.o: %.cpp
	$(CXX) -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fPIC \
-I$(SDK)/include -I$(CURDIR)/faust/architecture \
	       -Os -ffast-math -fdata-sections -ffunction-sections \
	       -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables \
	       -c $< -o $@

$(PLUGIN).o: $(OBJS)
	$(CXX) -r $^ -o $@

clean:
	rm -f ott_dsp.cpp $(OBJS) $(PLUGIN).o
