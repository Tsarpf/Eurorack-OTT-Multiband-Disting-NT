#include <math.h>
#include <new>
#include <distingnt/api.h>

// Algorithm state
struct _midiCcConverterAlgorithm_DTC
{
    // MIDI state
    int msb_value;     // Most significant byte value (0-127)
    int lsb_value;     // Least significant byte value (0-127)
    bool msb_received; // Flag to track if we've received an MSB message
    bool lsb_received; // Flag to track if we've received an LSB message

    // Output state
    float current_voltage;  // Current output voltage
    float previous_voltage; // Previous output voltage
    float smoothing_coef;   // Cached smoothing coefficient
};

struct _midiCcConverterAlgorithm : public _NT_algorithm
{
    _midiCcConverterAlgorithm(_midiCcConverterAlgorithm_DTC *dtc_) : dtc(dtc_)
    {
        dtc->msb_value = 0;
        dtc->lsb_value = 0;
        dtc->msb_received = false;
        dtc->lsb_received = false;
        dtc->current_voltage = 0.0f;
        dtc->smoothing_coef = 1.0f;
        dtc->previous_voltage = 0.0f;
    }

    _midiCcConverterAlgorithm_DTC *dtc;
};

// Parameter indices
enum
{
    kParamOutput,      // CV output
    kParamOutputMode,  // CV output mode (add/replace)
    kParamCC,          // CC number
    kParamMidiChannel, // MIDI channel (0 = all, 1-16 = specific)
    kParamBipolar,     // Bipolar mode (on/off)
    kParamSmoothing,   // Smoothing amount
};

// clang-format off
// Parameter definitions
_NT_parameter parameters[] = {
    NT_PARAMETER_CV_OUTPUT_WITH_MODE("CV Output", 1, 13)
    {.name = "CC", .min = 0, .max = 127, .def = 1, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL},
    {.name = "MIDI Ch", .min = 0, .max = 16, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL},
    {.name = "Bipolar", .min = 0, .max = 1, .def = 0, .unit = kNT_unitNone, .scaling = 0, .enumStrings = NULL},
    {.name = "Smoothing", .min = 0, .max = 100, .def = 0, .unit = kNT_unitPercent, .scaling = 0, .enumStrings = NULL},
};
// clang-format on

// Parameter pages
uint8_t page1[] = {kParamCC, kParamMidiChannel};
uint8_t page2[] = {kParamBipolar, kParamSmoothing};
uint8_t page3[] = {kParamOutput, kParamOutputMode};

_NT_parameterPage pages[] = {
    {.name = "Setup", .numParams = ARRAY_SIZE(page1), .params = page1},
    {.name = "CV", .numParams = ARRAY_SIZE(page2), .params = page2},
    {.name = "Routing", .numParams = ARRAY_SIZE(page3), .params = page3},
};

_NT_parameterPages parameterPages = {
    .numPages = ARRAY_SIZE(pages),
    .pages = pages,
};

// Calculate memory requirements for the algorithm
void calculateRequirements(_NT_algorithmRequirements &req, const int32_t* specifications)
{
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(_midiCcConverterAlgorithm);
    req.dram = 0;
    req.dtc = sizeof(_midiCcConverterAlgorithm_DTC);
    req.itc = 0;
}

// Construct the algorithm
_NT_algorithm *construct(const _NT_algorithmMemoryPtrs &ptrs, const _NT_algorithmRequirements &req, const int32_t* specifications)
{
    _midiCcConverterAlgorithm *alg = new (ptrs.sram) _midiCcConverterAlgorithm((_midiCcConverterAlgorithm_DTC *)ptrs.dtc);
    alg->parameters = parameters;
    alg->parameterPages = &parameterPages;
    return alg;
}

// Process incoming MIDI messages
void midiMessage(_NT_algorithm *self, uint8_t byte0, uint8_t byte1, uint8_t byte2)
{
    _midiCcConverterAlgorithm *pThis = (_midiCcConverterAlgorithm *)self;
    _midiCcConverterAlgorithm_DTC *dtc = pThis->dtc;

    // Only process CC messages (0xB0-0xBF)
    if ((byte0 & 0xF0) == 0xB0)
    {
        // Extract MIDI channel (0-15)
        int channel = byte0 & 0x0F;

        // Get the MIDI channel parameter (0 = all channels, 1-16 = specific channel)
        int midiChannel = pThis->v[kParamMidiChannel];

        // Skip if we're filtering by channel and this isn't our channel
        if (midiChannel > 0 && channel != (midiChannel - 1))
        {
            return;
        }

        // Extract CC number and value
        int cc_number = byte1;
        int cc_value = byte2;

        // Get MSB from parameters, LSB is always MSB + 32
        int msb_cc = pThis->v[kParamCC];
        int lsb_cc = msb_cc + 32;

        // Check for reset controllers message (CC 121) or all notes off (CC 123)
        if (cc_number == 121 || (cc_number == 123 && cc_value == 0))
        {
            dtc->msb_value = 0;
            dtc->lsb_value = 0;
            dtc->msb_received = false;
            dtc->lsb_received = false;
            dtc->current_voltage = 0.0f;
            return;
        }

        // Check if this is our MSB or LSB CC number
        if (cc_number == msb_cc)
        {
            dtc->msb_value = cc_value;
            dtc->msb_received = true;
        }
        else if (cc_number == lsb_cc)
        {
            dtc->lsb_value = cc_value;
            dtc->lsb_received = true;
        }

        // Calculate 14-bit value (0-16383) only when we have both values
        if (dtc->msb_received && dtc->lsb_received)
        {
            int value_14bit = (dtc->msb_value << 7) | dtc->lsb_value;

            // Normalize to 0.0-1.0 range
            float normalized = (float)value_14bit / 16383.0f;

            // Apply bipolar setting if enabled
            if (pThis->v[kParamBipolar])
            {
                // -5V to +5V range
                dtc->current_voltage = (normalized * 10.0f) - 5.0f;
            }
            else
            {
                // 0V to +10V range
                dtc->current_voltage = normalized * 10.0f;
            }

            // Reset flags for next pair of messages
            dtc->msb_received = false;
            dtc->lsb_received = false;
        }
    }
}

// Add parameter changed handler
void parameterChanged(_NT_algorithm *self, int p)
{
    _midiCcConverterAlgorithm *pThis = (_midiCcConverterAlgorithm *)self;
    _midiCcConverterAlgorithm_DTC *dtc = pThis->dtc;

    if (p == kParamSmoothing)
    {
        // Get smoothing value (0-100%)
        float smoothing = pThis->v[kParamSmoothing] / 100.0f;

        // Calculate smoothing coefficient using exponential approach
        if (smoothing > 0.0f)
        {
            // Convert percentage to time constant in milliseconds (1% = 1ms, 100% = 500ms)
            float timeConstantMs = smoothing * 500.0f;
            if (timeConstantMs < 1.0f)
                timeConstantMs = 1.0f;

            // Time constant formula: alpha = exp(-T/tau)
            // For a simple single-frame approach, T is 1/sampleRate
            float sampleRate = (float)NT_globals.sampleRate;
            float timeConstantSamples = (timeConstantMs / 1000.0f) * sampleRate;

            // This gives us a coefficient that determines how quickly we approach the target
            dtc->smoothing_coef = 1.0f - expf(-1.0f / timeConstantSamples);
        }
        else
        {
            dtc->smoothing_coef = 1.0f; // No smoothing
        }
    }
}

// DSP processing function
void step(_NT_algorithm *self, float *busFrames, int numFramesBy4)
{
    _midiCcConverterAlgorithm *pThis = (_midiCcConverterAlgorithm *)self;
    _midiCcConverterAlgorithm_DTC *dtc = pThis->dtc;
    int numFrames = numFramesBy4 * 4;

    // Get output destinations with bounds checking
    int cvOutputIdx = pThis->v[kParamOutput] - 1;
    if (cvOutputIdx < 0)
    {
        return; // Invalid output
    }

    float *cvOutput = busFrames + cvOutputIdx * numFrames;
    bool cvReplace = pThis->v[kParamOutputMode];

    float target_voltage = dtc->current_voltage; // x[n] - target to move towards
    float filter_state = dtc->previous_voltage;  // y[n-1] - current filter state

    for (int i = 0; i < numFrames; ++i)
    {
        // IIR One-pole lowpass filter: y[n] = y[n-1] + α(x[n] - y[n-1])
        filter_state = filter_state + (dtc->smoothing_coef * (target_voltage - filter_state));

        // Output to CV
        if (cvReplace)
        {
            cvOutput[i] = filter_state;
        }
        else
        {
            cvOutput[i] += filter_state;
        }
    }

    // Save the filter state for next time
    dtc->previous_voltage = filter_state;
}

// Factory information
_NT_factory factory = {
    .guid = NT_MULTICHAR('T', 'h', '1', '4'),
    .name = "14-bit CC to CV",
    .description = "Converts 14-bit MIDI CC messages to CV",
	.numSpecifications = 0,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = NULL,
    .midiMessage = midiMessage,
};

// Plugin entry point
uintptr_t pluginEntry(_NT_selector selector, uint32_t data)
{
    switch (selector)
    {
    case kNT_selector_version:
        return kNT_apiVersionCurrent;
    case kNT_selector_numFactories:
        return 1;
    case kNT_selector_factoryInfo:
        return (uintptr_t)((data == 0) ? &factory : NULL);
    }
    return 0;
}
