
#include <math.h>
#include <new>
#include <distingnt/api.h>

struct _algorithm1 : public _NT_algorithm
{
	_algorithm1() {}
	~_algorithm1() {}
};

struct _algorithm2 : public _NT_algorithm
{
	_algorithm2() {}
	~_algorithm2() {}
};

void	calculateRequirements1( _NT_algorithmRequirements& req, const int32_t* specifications )
{
	req.numParameters = 0;
	req.sram = sizeof(_algorithm1);
	req.dram = 0;
	req.dtc = 0;
	req.itc = 0;
}

void	calculateRequirements2( _NT_algorithmRequirements& req, const int32_t* specifications )
{
	req.numParameters = 0;
	req.sram = sizeof(_algorithm2);
	req.dram = 0;
	req.dtc = 0;
	req.itc = 0;
}

_NT_algorithm*	construct1( const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& req, const int32_t* specifications )
{
	_algorithm1* alg = new (ptrs.sram) _algorithm1();
	return alg;
}

_NT_algorithm*	construct2( const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements& req, const int32_t* specifications )
{
	_algorithm2* alg = new (ptrs.sram) _algorithm2();
	return alg;
}

void 	step1( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
	int numFrames = numFramesBy4 * 4;
	float* out = busFrames + ( 15 - 1 ) * numFrames;
	
	for ( int i=0; i<numFrames; ++i )
	{
		out[i] = 1.0f;
	}
}

void 	step2( _NT_algorithm* self, float* busFrames, int numFramesBy4 )
{
	int numFrames = numFramesBy4 * 4;
	float* out = busFrames + ( 16 - 1 ) * numFrames;
	
	for ( int i=0; i<numFrames; ++i )
	{
		out[i] = -1.0f;
	}
}

static const _NT_factory factory1 = 
{
	.guid = NT_MULTICHAR( 'E', 'x', 'm', '1' ),
	.name = "Multiple 1",
	.description = "Test of multiple factories, pt1",
	.numSpecifications = 0,
	.calculateRequirements = calculateRequirements1,
	.construct = construct1,
	.parameterChanged = NULL,
	.step = step1,
	.draw = NULL,
	.midiMessage = NULL,
};

static const _NT_factory factory2 = 
{
	.guid = NT_MULTICHAR( 'E', 'x', 'm', '2' ),
	.name = "Multiple 2",
	.description = "Test of multiple factories, pt2",
	.numSpecifications = 0,
	.calculateRequirements = calculateRequirements2,
	.construct = construct2,
	.parameterChanged = NULL,
	.step = step2,
	.draw = NULL,
	.midiMessage = NULL,
};

uintptr_t pluginEntry( _NT_selector selector, uint32_t data )
{
	switch ( selector )
	{
	case kNT_selector_version:
		return kNT_apiVersionCurrent;
	case kNT_selector_numFactories:
		return 2;
	case kNT_selector_factoryInfo:
		if ( data == 0 )
			return (uintptr_t)&factory1;
		if ( data == 1 )
			return (uintptr_t)&factory2;
		return 0;
	}
	return 0;
}
