
ifndef NT_API_PATH
	export NT_API_PATH = ../..
endif

export FAUSTARCH = ../

inputs := $(wildcard *dsp)
outputs := $(patsubst %.dsp,plugins/%.o,$(inputs))

all: $(outputs)

clean:
	rm -f $(outputs)

plugins/%.o: %.dsp
	mkdir -p $(@D)
	../faust2distingnt $^ $@
