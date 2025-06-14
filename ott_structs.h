#pragma once
#include "ott_ui.h"        // brings ParamUI, UIState etc.
#include "ott_dsp.cpp"

/* full definition must be visible in both translation units */
struct _ottAlgorithm : public _NT_algorithm
{
    FaustDsp dsp;
    ParamUI  ui;
    UIState  state;
};
