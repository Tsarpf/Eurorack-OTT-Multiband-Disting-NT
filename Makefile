# Adjust these two paths to match your SDK install ------------------------------------------------
SDK      ?= $(CURDIR)/distingnt_api
FAUST    ?= faust

PLUGIN   := ott
SRCS     := ott_dsp.cpp ott_wrapper.cpp
OBJ      := $(PLUGIN).o

# Build rules ------------------------------------------------------------------
all: $(OBJ)

ott_dsp.cpp: ott.dsp
	$(FAUST) -a $(SDK)/faust/nt_arch.cpp $< -o $@

$(OBJ): $(SRCS)
	$(CXX) -std=c++17 -I$(SDK)/distingnt -Os -ffast-math -fdata-sections -ffunction-sections -Wl,--gc-sections -c $^ -o $@

clean:
	rm -f ott_dsp.cpp $(OBJ)
