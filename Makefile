SDK      ?= $(CURDIR)/distingnt_api/
FAUST    ?= faust

PLUGIN   := ott
SRCS     := ott_dsp.cpp
OBJS     := $(SRCS:.cpp=.o)

CXX      = arm-none-eabi-g++

all: $(PLUGIN).o

ott_dsp.cpp: ott.dsp
	$(FAUST) -a $(SDK)/faust/nt_arch.cpp -uim -nvi -mem $< -o $@

%.o: %.cpp
	$(CXX) -std=c++11 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -fPIC \
	       -I$(SDK)/include \
	       -Os -ffast-math -fdata-sections -ffunction-sections \
	       -fno-exceptions -fno-rtti -fno-unwind-tables -fno-asynchronous-unwind-tables \
	       -c $< -o $@

$(PLUGIN).o: $(OBJS)
	$(CXX) -r $^ -o $@

clean:
	rm -f ott_dsp.cpp $(OBJS) $(PLUGIN).o
