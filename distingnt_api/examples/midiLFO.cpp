/*
MIT License

Copyright (c) 2025 Expert Sleepers Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <math.h>
#include <new>
#include <distingnt/api.h>

struct _midiLFO : public _NT_algorithm
{
	_midiLFO() {}
	~_midiLFO() {}
	
	int32_t		countdown;
	uint32_t	value;
};

enum
{
	kParamType,
	kParamCC,
};

static char const * const enumStringsType[] = {
	"CC",
	"Aftertouch",
	"Clock",
};

static const _NT_parameter	parameters[] = {
	{ .name = "Type", .min = 0, .max = 2, .def = 0, .unit = kNT_unitEnum, .scaling = 0, .enumStrings = enumStringsType },
	{ .name = "CC", .min = 0, .max = 127, .def = 7, .unit = 0, .scaling = 0, .enumStrings = NULL },
};

static const uint8_t page1[] = { kParamType, kParamCC };

static const _NT_parameterPage pages[] = {
	{ .name = "LFO", .numParams = ARRAY_SIZE(page1), .params = page1 },
};

static const _NT_parameterPages parameterPages = {
	.numPages = ARRAY_SIZE(pages),
	.pages = pages,
};

void	calculateRequirements( _NT_algorithmRequirements& req, const int32_t* specifications )
{
	req.numParameters = ARRAY_SIZE(parameters);
	req.sram = sizeof(_midiLFO);
	req.dram = 0;
	req.dtc = 0;
	req.itc = 0;
}

_NT_algorithm*	construct( const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& req, const int32_t* specifications )
{
	_midiLFO* alg = new (ptrs.sram) _midiLFO();
	alg->parameters = parameters;
	alg->parameterPages = &parameterPages;
	return alg;
}

void 	step( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
	_midiLFO* pThis = (_midiLFO*)self;
	
	pThis->countdown -= 4 * numFramesBy4;
	
	if ( pThis->countdown <= 0 )
	{
		// new message at 2Hz
		pThis->countdown = NT_globals.sampleRate / 2;
		
		switch ( pThis->v[kParamType] )
		{
		default:
		case 0:
			NT_sendMidi3ByteMessage( ~0, 0xb0, pThis->v[kParamCC], pThis->value );
			break;
		case 1:
			NT_sendMidi2ByteMessage( ~0, 0xd0, pThis->value );
			break;
		case 2:
			NT_sendMidiByte( ~0, 0xf8 );
			break;
		}
		
		pThis->value = ( pThis->value + 1 ) & 127;
	}
}

static const _NT_factory factory = 
{
	.guid = NT_MULTICHAR( 'E', 'x', 'm', 'l' ),
	.name = "MIDI LFO",
	.description = "Emits MIDI",
	.numSpecifications = 0,
	.calculateRequirements = calculateRequirements,
	.construct = construct,
	.step = step,
	.tags = kNT_tagUtility,
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
