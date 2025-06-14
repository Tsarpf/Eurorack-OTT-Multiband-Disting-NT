#pragma once
#include <cstdint>

struct Meta  { virtual ~Meta() = default; };
struct UI    { virtual ~UI() = default;   };

class dsp
{
public:
    virtual ~dsp()                         {}
    virtual int  getNumInputs()            = 0;
    virtual int  getNumOutputs()           = 0;
    virtual void metadata(Meta*)           = 0;
    virtual void buildUserInterface(UI*)   = 0;
    virtual void init(int sample_rate)     = 0;
    virtual void instanceInit(int sr)      = 0;
    virtual void instanceConstants(int sr) = 0;
    virtual void instanceResetUserInterface() = 0;
    virtual void instanceClear()           = 0;
    virtual int  getSampleRate()           = 0;
    virtual dsp* clone()                   = 0;
    virtual void compute(int count,
                         float** inputs,
                         float** outputs)  = 0;
};
