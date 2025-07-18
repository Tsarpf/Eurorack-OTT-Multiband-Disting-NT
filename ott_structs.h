#pragma once

#include <distingnt/api.h>
#include "ott_ui.h"        // brings ParamUI, UIState
#include "newlib_heap.h"

class FaustDsp;            // defined in ott_dsp.cpp

/* per-instance struct */
struct _ottAlgorithm : public _NT_algorithm
{
    PlugHeap heap;
    FaustDsp* dsp = nullptr;
    ParamUI   ui;
    UIState   state;
    int       lastParam = -1;
    int16_t   lastValue = 0;
    float     potCatch[3] = {0.f,0.f,0.f};
    bool      potCaught[3] = {false,false,false};
    int       potTarget[3] = {-1,-1,-1};
    bool      potUpper[3] = {false,false,false};
};
