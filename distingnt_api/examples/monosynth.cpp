
#include <math.h>
#include <new>
#include <distingnt/api.h>

struct _monosynthAlgorithm_DTC
{
	uint32_t	phase;
	uint32_t	inc;
	float 		env;
	bool		gate;
};

struct _monosynthAlgorithm : public _NT_algorithm
{
	_monosynthAlgorithm( _monosynthAlgorithm_DTC* dtc_ ) : dtc( dtc_ ) {}
	~_monosynthAlgorithm() {}
	
	_monosynthAlgorithm_DTC*	dtc;
};

enum
{
	kParamOutput,
	kParamOutputMode,
	kParamMidiChannel,
	kParamWaveform,
};

static char const * const enumStringsWaveform[] = {
	"Square",
	"Sawtooth",
};

static const _NT_parameter	parameters[] = {
	NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE( "Output", 1, 13 )
	{ .name = "MIDI channel", .min = 1, .max = 16, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL },
	{ .name = "Waveform", .min = 0, .max = 1, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumStringsWaveform },
};

static const uint8_t page1[] = { kParamMidiChannel, kParamWaveform };
static const uint8_t page2[] = { kParamOutput, kParamOutputMode };

static const _NT_parameterPage pages[] = {
	{ .name = "Setup", .numParams = ARRAY_SIZE(page1), .params = page1 },
	{ .name = "Routing", .numParams = ARRAY_SIZE(page2), .params = page2 },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

void	calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
	req.numParameters = ARRAY_SIZE(parameters);
	req.sram = sizeof(_monosynthAlgorithm);
	req.dram = 0;
	req.dtc = sizeof(_monosynthAlgorithm_DTC);
	req.itc = 0;
}

_NT_algorithm*	construct( const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& req, const int32_t* specifications )
{
	_monosynthAlgorithm* alg = new (ptrs.sram) _monosynthAlgorithm( (_monosynthAlgorithm_DTC*)ptrs.dtc );
	alg->parameters = parameters;
	alg->parameterPages = &parameterPages;
	return alg;
}

void	midiMessage( _NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2 )
{
	_monosynthAlgorithm* pThis = (_monosynthAlgorithm*)self;
	_monosynthAlgorithm_DTC* dtc = pThis->dtc;
	
	int status = byte0 & 0xf0;
	switch ( status )
	{
        case 0x80:      // note off
	    case 0x90:      // note on
			if ( ( byte0 & 0xf ) != ( pThis->v[kParamMidiChannel] - 1 ) )
    	    	break;
    	    if ( status == 0x80 || byte2 == 0 )
    	    {
    	    	// note off
    	    	dtc->gate = false;
    	    }
    	    else
    	    {
    	    	// note on
    	    	dtc->gate = true;
    	    	dtc->inc = exp2( ( byte1 - 69 )/12.0f ) * 440.0f * (float)(1ULL<<32) / NT_globals.sampleRate;
    	    }
        	break;
	}
}

void 	step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
	_monosynthAlgorithm* pThis = (_monosynthAlgorithm*)self;
	_monosynthAlgorithm_DTC* dtc = pThis->dtc;
	
	int numFrames = numFramesBy4 * 4;
	float* out = busFrames + ( pThis->v[kParamOutput] - 1 ) * numFrames;
	bool replace = pThis->v[kParamOutputMode];
	
	float target = dtc->gate ? 1.0f : 0.0f;
	bool saw = pThis->v[kParamWaveform];
	
	for ( int i=0; i<numFrames; ++i )
	{
		dtc->phase += dtc->inc;
		
		float v;
		if ( saw )
			v = (int)dtc->phase * (5.0f/(1<<31));
		else
			v = (int)dtc->phase < 0 ? -5.0f : 5.0f;
	
		dtc->env = target + 0.99f * ( dtc->env - target );
	
		if ( !replace )
			out[i] += v * dtc->env;
		else
			out[i] = v * dtc->env;
	}
}

static const _NT_factory factory = 
{
	.guid = NT_MULTICHAR( 'E', 'x', 'm', 's' ),
	.name = "Monosynth",
	.description = "Very simple MIDI synth",
	.numSpecifications = 0,
	.calculateRequirements = calculateRequirements,
	.construct = construct,
	.parameterChanged = NULL,
	.step = step,
	.draw = NULL,
	.midiMessage = midiMessage,
};

uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
	switch ( selector )
	{
	case kNT_selector_version:
		return kNT_apiVersionCurrent;
	case kNT_selector_numFactories:
		return 1;
	case kNT_selector_factoryInfo:
		return (uintptr_t)( ( data == 0 ) ? &factory : NULL );
	}
	return 0;
}
