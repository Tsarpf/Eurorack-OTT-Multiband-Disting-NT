# Adjust these two paths to match your SDK install -----------------------------
SDK      ?= $(CURDIR)/distingnt_api/
FAUST    ?= faust

PLUGIN   := ott
SRCS     := ott_dsp.cpp ott_wrapper.cpp
OBJS     := $(SRCS:.cpp=.o)

# Build rules ------------------------------------------------------------------
all: $(PLUGIN).o

ott_dsp.cpp: ott.dsp
	$(FAUST) -a $(SDK)/faust/nt_arch.cpp $< -o $@

%.o: %.cpp
	$(CXX) -std=c++17 -I$(SDK)/include \
	       -Os -ffast-math -fdata-sections -ffunction-sections \
	       -c $< -o $@

$(PLUGIN).o: $(OBJS)
	$(CXX) -r $^ -o $@

clean:
	rm -f ott_dsp.cpp $(OBJS) $(PLUGIN).o
