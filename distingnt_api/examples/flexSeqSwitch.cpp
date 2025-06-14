/*
 * Sequential Switch (Flex)
 * Author: Thorinside / Idea by Jodok31283 / Code by AI
 */

#include <distingnt/api.h>
#include <new>     // for placement new
#include <cstddef> // for size_t (though not strictly needed here)

// --- Use constants for specification indices ---
enum SpecIndex
{
    SPEC_CHANNELS = 0,
    SPEC_INPUTS = 1,
    SPEC_OUTPUTS = 2,
    NUM_SPECS // Keep track of count
};

// --- Use constants for hardware limits ---
constexpr int MAX_INPUT_GROUPS = 12;
constexpr int MAX_OUTPUT_GROUPS = 8;
constexpr int MAX_BUSSES = 28; // Max selectable bus number

// --- Constants ---

// Use constants for array sizes
constexpr size_t MAX_PARAMS_POSSIBLE = (2 /*Sig L+R*/ + 2 /*Clk+Rst*/ + MAX_OUTPUT_GROUPS * 4 /*OutL+ModeL+OutR+ModeR*/);
static_assert(MAX_PARAMS_POSSIBLE == 36, "Max params calculation mismatch");

// Maximum possible parameters: Base (36) + AntiClick (1)
constexpr size_t MAX_PARAMS_ACTUAL = MAX_PARAMS_POSSIBLE + 1;
static_assert(MAX_PARAMS_ACTUAL <= 40, "Total parameters exceed reasonable limit"); // Example check

// --- Parameter Indices Enum ---
enum ParamIndex
{
    // Indices will be assigned dynamically in construct based on mode
    // Placeholders for clarity in step function logic
    PARAM_IDX_INVALID = -1,

    // Demux Input Signals (Indices 0, [1])
    PARAM_IDX_DEMUX_SIG_L_IN = 0,
    PARAM_IDX_DEMUX_SIG_R_IN = 1,

    // Mux Output Signals (Indices vary)
    PARAM_IDX_MUX_SIG_L_OUT,
    PARAM_IDX_MUX_SIG_L_MODE,
    PARAM_IDX_MUX_SIG_R_OUT,
    PARAM_IDX_MUX_SIG_R_MODE,

    // Shared Clock/Reset (Indices vary)
    PARAM_IDX_CLK_IN,
    PARAM_IDX_RST_IN,

    // Anti-Click Toggle (Last parameter)
    PARAM_IDX_ANTI_CLICK
};

/* ───── specifications ───── */
static const _NT_specification gSpecs[] = {
    {.name = "Channels", .min = 1, .max = 2, .def = 1, .type = kNT_typeGeneric},               // SPEC_CHANNELS
    {.name = "Inputs", .min = 1, .max = MAX_INPUT_GROUPS, .def = 1, .type = kNT_typeGeneric},  // SPEC_INPUTS
    {.name = "Outputs", .min = 1, .max = MAX_OUTPUT_GROUPS, .def = 2, .type = kNT_typeGeneric} // SPEC_OUTPUTS
};
static_assert(NUM_SPECS == sizeof(gSpecs) / sizeof(gSpecs[0]), "SpecIndex enum size mismatch");

/* short fixed-name tables */
// Add const
static const char *const monoIn[MAX_INPUT_GROUPS] = { // Use constant size
    "Input1", "Input2", "Input3", "Input4", "Input5", "Input6",
    "Input7", "Input8", "Input9", "Input10", "Input11", "Input12"};

// Add const
static const char *const monoOut[MAX_OUTPUT_GROUPS] = { // Use constant size
    "Output1", "Output2", "Output3", "Output4",
    "Output5", "Output6", "Output7", "Output8"};

// Add const
static const char *const sterInL[MAX_INPUT_GROUPS] = { // Use constant size
    "Input1 L", "Input2 L", "Input3 L", "Input4 L", "Input5 L", "Input6 L",
    "Input7 L", "Input8 L", "Input9 L", "Input10 L", "Input11 L", "Input12 L"};
// Add const
static const char *const sterInR[MAX_INPUT_GROUPS] = { // Use constant size
    "Input1 R", "Input2 R", "Input3 R", "Input4 R", "Input5 R", "Input6 R",
    "Input7 R", "Input8 R", "Input9 R", "Input10 R", "Input11 R", "Input12 R"};

// Add const
static const char *const sterOutL[MAX_OUTPUT_GROUPS] = { // Use constant size
    "Output1 L", "Output2 L", "Output3 L", "Output4 L",
    "Output5 L", "Output6 L", "Output7 L", "Output8 L"};
// Add const
static const char *const sterOutR[MAX_OUTPUT_GROUPS] = { // Use constant size
    "Output1 R", "Output2 R", "Output3 R", "Output4 R",
    "Output5 R", "Output6 R", "Output7 R", "Output8 R"}; // Fixed typo

/* helper to fill a parameter */
// Add const char* n
static inline void setP(_NT_parameter &p, const char *n, int16_t min, int16_t max,
                        int16_t def, uint8_t unit)
{
    p.name = n;
    p.min = min;
    p.max = max;
    p.def = def;
    p.unit = unit;
    p.scaling = 0;
    p.enumStrings = nullptr; // Explicit nullptr
}

/* ───── instance ───── */
struct Switch : _NT_algorithm
{
    // --- Configuration ---
    uint8_t lanes, gIn, gOut;
    bool demux;

    // --- Runtime State ---
    uint8_t active_index; // Current active index
    bool clock_high, reset_high;

    // --- Fade State ---
    bool is_fading_out = false;
    bool is_fading_in = false;
    uint8_t pending_active_index; // Index switching TO
    uint32_t fade_samples_remaining;
    uint32_t current_fade_samples; // Set by parameter, default 0
    float fade_sample_reciprocal;  // Cached 1.0 / current_fade_samples

    // --- Parameter Storage ---
    uint8_t num_params_actual;
    _NT_parameter params[MAX_PARAMS_ACTUAL];
    uint8_t page_idx_routing[MAX_PARAMS_POSSIBLE];
    uint8_t page_idx_audio[1];
    _NT_parameterPage page_defs[2];
    _NT_parameterPages pages;

    // --- Parameter Index Mapping (set in construct) ---
    int param_idx_clk = PARAM_IDX_INVALID;
    int param_idx_rst = PARAM_IDX_INVALID;
    int param_idx_mux_out_l = PARAM_IDX_INVALID;
    int param_idx_mux_out_r = PARAM_IDX_INVALID;
    int param_idx_fade_samples = PARAM_IDX_INVALID;
    int first_group_input_param = PARAM_IDX_INVALID;
    int first_group_output_param = PARAM_IDX_INVALID;

    // Constructor can initialize members if desired (though construct() does it)
    Switch() : lanes(0), gIn(0), gOut(0), demux(false),
               active_index(0), clock_high(false), reset_high(false),
               is_fading_out(false), is_fading_in(false), pending_active_index(0),
               fade_samples_remaining(0), current_fade_samples(0), fade_sample_reciprocal(0.0f),
               num_params_actual(0), page_defs{}, pages{} {}
};

/* helpers */
// Use const float* b, const int N? No, buffer is modified by algorithm eventually.
// Use size_t N? API uses int. Stick to int.
static inline float *bus(float *b, int bus_idx, int N)
{
    // Use MAX_BUSSES? API uses 28 directly in param definitions.
    // Keep check simple as before.
    return (bus_idx > 0) ? (b + (bus_idx - 1) * N) : nullptr;
}
// Use const float v
static inline bool rise(const float v, bool &state)
{
    const bool high = v > 1.f;
    const bool rising_edge = high && !state;
    state = high;
    return rising_edge;
}

/* ───── requirements ───── */
// Use const for sp pointer
static void calcReq(_NT_algorithmRequirements &r, const int32_t *const sp)
{
    // Use enum for spec indices
    const int lanes = sp[SPEC_CHANNELS];
    const int gIn = sp[SPEC_INPUTS];
    const int gOut = sp[SPEC_OUTPUTS];
    const bool demux = (gIn == 1);
    int num_params = 0; // Calculate locally

    if (demux)
    { // 1 -> N
        if (lanes == 1)
        {
            num_params = 1 /*Sig*/ + 2 /*Clk+Rst*/ + gOut * 2 /*Out+Mode*/;
        }
        else
        { // lanes == 2
            num_params = 2 /*Sig L+R*/ + 2 /*Clk+Rst*/ + gOut * 4 /*OutL+ModeL+OutR+ModeR*/;
        }
    }
    else
    { // N -> 1
        if (lanes == 1)
        {
            num_params = gIn * 1 /*In*/ + 2 /*Clk+Rst*/ + 2 /*SigOut+Mode*/;
        }
        else
        { // lanes == 2
            num_params = gIn * 2 /*InL+InR*/ + 2 /*Clk+Rst*/ + 4 /*SigOutL+ModeL+SigOutR+ModeR*/;
        }
    }

    // Add the Anti-Click parameter
    num_params += 1;

    // Validation using constants
    const bool invalid_config = (lanes * gIn > MAX_INPUT_GROUPS) ||
                                (lanes * gOut > MAX_OUTPUT_GROUPS) ||
                                (gIn == 1) == (gOut == 1); // XOR implies gIn=1 AND gOut=1, or gIn!=1 AND gOut!=1

    // Report total calculated parameters
    r.numParameters = invalid_config ? 0 : num_params; // Let's signal invalidity with 0 params

    // Ensure it doesn't exceed our adjusted max
    if (r.numParameters > MAX_PARAMS_ACTUAL)
    {
        r.numParameters = MAX_PARAMS_ACTUAL; // Safety clamp
    }

    r.sram = sizeof(Switch);
    r.dram = 0;
    r.dtc = 0;
    r.itc = 0;
}

// Forward declaration for parameterChanged
static void parameterChanged(_NT_algorithm *self_base, int p);

/* ───── constructor ───── */
// Use const for pointers where possible
static _NT_algorithm *construct(const _NT_algorithmMemoryPtrs &m,
                                const _NT_algorithmRequirements &r,
                                const int32_t *const sp)
{
    // Use enum for spec indices
    const uint8_t lanes = sp[SPEC_CHANNELS];
    const uint8_t gIn = sp[SPEC_INPUTS];
    const uint8_t gOut = sp[SPEC_OUTPUTS];

    // Validate config again (redundant with calcReq if it sets numParams=0, but safe)
    if ((lanes * gIn > MAX_INPUT_GROUPS) || (lanes * gOut > MAX_OUTPUT_GROUPS) || (gIn == 1) == (gOut == 1) || r.numParameters == 0)
    {
        return nullptr; // Invalid configuration
    }

    Switch *self = new (m.sram) Switch(); // Use constructor init + placement new

    self->lanes = lanes;
    self->gIn = gIn;
    self->gOut = gOut;
    self->demux = (gIn == 1);
    self->active_index = 0;
    self->clock_high = false;
    self->reset_high = false;
    // self->num_params_actual will be set below

    int p = 0; // Parameter index counter

    /* Inputs */
    if (self->demux)
    { // 1 -> N : Define Sig L/[R] inputs
        // PARAM_IDX_DEMUX_SIG_L_IN is implicitly 0
        setP(self->params[p++], "Sig Left Input", 1, MAX_BUSSES, 1, kNT_unitAudioInput);
        if (self->lanes == 2)
        {
            // PARAM_IDX_DEMUX_SIG_R_IN is implicitly 1
            setP(self->params[p++], "Sig Right Input", 0, MAX_BUSSES, 0, kNT_unitAudioInput); // Allow 'None' for Right? Or default to 2? Let's keep 0.
        }
    }
    else
    {                                      // N -> 1 : Define group inputs
        self->first_group_input_param = p; // Store index of "Input 1 L"
        for (int g = 0; g < self->gIn; ++g)
        {
            setP(self->params[p++], self->lanes == 1 ? monoIn[g] : sterInL[g], 0, MAX_BUSSES, 0, kNT_unitAudioInput); // Allow 'None', default 0
            if (self->lanes == 2)
            {
                setP(self->params[p++], sterInR[g], 0, MAX_BUSSES, 0, kNT_unitAudioInput); // Allow 'None', default 0
            }
        }
    }

    /* Clock / Reset */
    self->param_idx_clk = p;                                             // Store Clock index
    setP(self->params[p++], "Clock", 0, MAX_BUSSES, 0, kNT_unitCvInput); // Allow 'None', default 0
    self->param_idx_rst = p;                                             // Store Reset index
    setP(self->params[p++], "Reset", 0, MAX_BUSSES, 0, kNT_unitCvInput); // Allow 'None', default 0

    /* Outputs */
    if (self->demux)
    {                                       // 1 -> N : Define group outputs
        self->first_group_output_param = p; // Store index of "Output 1 L"
        for (int g = 0; g < self->gOut; ++g)
        {
            setP(self->params[p++], self->lanes == 1 ? monoOut[g] : sterOutL[g], 1, MAX_BUSSES, 1, kNT_unitAudioOutput); // Force selection, default 1
            setP(self->params[p++], "mode", 0, 1, 0, kNT_unitOutputMode);                                                // Mode L
            if (self->lanes == 2)
            {
                // Check index calculation again for sterOutR - fixed typo earlier
                setP(self->params[p++], sterOutR[g], 1, MAX_BUSSES, 1, kNT_unitAudioOutput); // Force selection, default 1
                setP(self->params[p++], "mode", 0, 1, 0, kNT_unitOutputMode);                // Mode R
            }
        }
    }
    else
    {                                                                                      // N -> 1 : Define Sig L/[R] outputs
        self->param_idx_mux_out_l = p;                                                     // Store Sig L Out index
        setP(self->params[p++], "Sig Left Output", 1, MAX_BUSSES, 1, kNT_unitAudioOutput); // Force selection, default 1
        setP(self->params[p++], "Sig Left mode", 0, 1, 0, kNT_unitOutputMode);
        if (self->lanes == 2)
        {
            self->param_idx_mux_out_r = p;                                                      // Store Sig R Out index
            setP(self->params[p++], "Sig Right Output", 1, MAX_BUSSES, 1, kNT_unitAudioOutput); // Force selection, default 1
            setP(self->params[p++], "Sig Right mode", 0, 1, 0, kNT_unitOutputMode);
        }
    }

    const int num_routing_params = p; // Number of parameters defined so far

    // --- Define Audio Parameters ---
    self->param_idx_fade_samples = p;        // Store index of this param
    const uint16_t default_fade_samples = 0; // Reset default to 0 = OFF
    const uint16_t max_fade_samples = 512;   // Reset max fade duration (~10.7ms @ 48k)
    setP(self->params[p++], "Fade Samples", 0, max_fade_samples, default_fade_samples, kNT_unitFrames);
    // No enum strings needed

    // --- Final Setup ---
    self->num_params_actual = p;

    // --- Parameter Pages ---
    // Routing Page
    int routing_page_param_count = 0;
    for (int i = 0; i < num_routing_params; ++i)
    {
        self->page_idx_routing[routing_page_param_count++] = i;
    }
    self->page_defs[0] = {.name = "Routing", .numParams = (uint8_t)routing_page_param_count, .params = self->page_idx_routing}; // Use designated initializers

    // Audio Page
    self->page_idx_audio[0] = self->param_idx_fade_samples;
    self->page_defs[1] = {.name = "Audio", .numParams = 1, .params = self->page_idx_audio}; // Use designated initializers

    // Assign pages struct
    self->pages = {.numPages = 2, .pages = self->page_defs}; // Use designated initializers

    self->parameters = self->params;
    self->parameterPages = &self->pages;

    // Initialize fade state
    self->fade_samples_remaining = 0;
    self->current_fade_samples = default_fade_samples;
    self->fade_sample_reciprocal = (default_fade_samples > 0) ? (1.0f / (float)default_fade_samples) : 0.0f;
    self->is_fading_out = false;
    self->is_fading_in = false;
    self->pending_active_index = 0;

    // NOTE: Relying on host to call parameterChanged after construct if needed for presets/defaults.
    return self;
}

// Update internal state when parameters change
static void parameterChanged(_NT_algorithm *self_base, int p)
{
    Switch *self = static_cast<Switch *>(self_base); // Rename mutable_self to self

    if (p == self->param_idx_fade_samples)
    {                                  // Check if "Fade Samples" param changed
        uint32_t samples = self->v[p]; // Use self
        self->current_fade_samples = samples;
        self->fade_sample_reciprocal = (samples > 0) ? (1.0f / (float)samples) : 0.0f;
    }
}

/* ---- DSP: copy audio/CV with Add / Replace ---- */
// Structure to hold side lane info
struct Lane
{
    float *ptr = nullptr; // Initialize to nullptr
    uint8_t replace = 1;  // Default to replace (doesn't matter for input mux)
};

// Use const int nBy4
static void step(_NT_algorithm *b, float *buf, const int nBy4)
{
    // Casts
    Switch *self = static_cast<Switch *>(b); // Rename mutable_self to self
    const int N = nBy4 * 4;

    // --- Get Clock/Reset pointers ---
    const int bus_idx_clk = self->v[self->param_idx_clk];
    const int bus_idx_rst = self->v[self->param_idx_rst];
    float *clk = bus(buf, bus_idx_clk, N);
    float *rst = bus(buf, bus_idx_rst, N);

    // --- Get Main Output pointers (Mux mode only) ---
    float *outL = nullptr;
    float *outR = nullptr;
    if (!self->demux)
    {
        outL = bus(buf, self->v[self->param_idx_mux_out_l], N);
        if (self->lanes == 2)
        {
            outR = bus(buf, self->v[self->param_idx_mux_out_r], N);
        }
    }
    // --- Get Main Input pointers (Demux mode only) ---
    float *inL = nullptr;
    float *inR = nullptr;
    if (self->demux)
    {
        inL = bus(buf, self->v[PARAM_IDX_DEMUX_SIG_L_IN], N);
        if (self->lanes == 2)
        {
            inR = bus(buf, self->v[PARAM_IDX_DEMUX_SIG_R_IN], N);
        }
    }

    /* ------------------------------------------------------------------ */
    /* Build array of side lanes (pointers only needed now)             */
    /* ------------------------------------------------------------------ */
    const int groups = self->demux ? self->gOut : self->gIn;
    float *side_ptr[MAX_INPUT_GROUPS][2] = {}; // Pointers only

    // Calculate starting parameter index for side lanes
    int param_idx_side_start = self->demux ? self->first_group_output_param
                                           : self->first_group_input_param;
    int current_param_idx = param_idx_side_start;

    for (int g = 0; g < groups; ++g)
    {
        const int bus_idx_l = self->v[current_param_idx];
        side_ptr[g][0] = bus(buf, bus_idx_l, N);
        current_param_idx += self->demux ? 2 : 1;

        if (self->lanes == 2)
        {
            const int bus_idx_r = self->v[current_param_idx];
            side_ptr[g][1] = bus(buf, bus_idx_r, N);
            current_param_idx += self->demux ? 2 : 1;
        }
    }

    /* ------------------------------------------------------------------ */
    /* Audio-rate loop                                                    */
    /* ------------------------------------------------------------------ */
    for (int n = 0; n < N; ++n)
    {
        /* --- Clock & Reset Logic --- */
        if (clk && rise(clk[n], self->clock_high))
        {
            // Clock edge: Trigger a switch if currently idle
            if (!self->is_fading_out && !self->is_fading_in)
            {
                if (self->current_fade_samples > 0 && !self->demux)
                {
                    // Start fade OUT (don't switch active_index yet)
                    self->pending_active_index = (self->active_index + 1) % groups;
                    self->is_fading_out = true;
                    self->fade_samples_remaining = self->current_fade_samples;
                }
                else
                {
                    // Instant switch (fade=0 or Demux mode)
                    self->active_index = (self->active_index + 1) % groups;
                }
            }
            // Else: Ignore clock edges during fade (implicit)
        }
        if (rst && rise(rst[n], self->reset_high))
        {
            // Reset edge: Always reset instantly, cancel fade
            self->active_index = 0;
            self->is_fading_out = false;
            self->is_fading_in = false;
        }

        // --- Determine gain and index based on state ---
        float current_gain = 1.0f;
        uint8_t index_to_use = self->active_index;

        if (self->is_fading_out)
        {
            if (self->fade_samples_remaining > 0)
            {
                self->fade_samples_remaining--;
                // Calculate fade out gain (1 -> 0) using reciprocal
                current_gain = (float)self->fade_samples_remaining * self->fade_sample_reciprocal;
                index_to_use = self->active_index; // Still using the old index
            }
            else
            {
                // Fade out finished: Switch index, start fade in
                self->active_index = self->pending_active_index;
                self->is_fading_out = false;
                self->is_fading_in = true;
                self->fade_samples_remaining = self->current_fade_samples; // Reset counter using original samples value
                index_to_use = self->active_index;                         // Use the NEW index now
                // Calculate gain for the FIRST sample of fade-in using reciprocal
                current_gain = 1.0f - (float)self->fade_samples_remaining * self->fade_sample_reciprocal;
                // Decrement counter for this first sample of fade-in
                if (self->fade_samples_remaining > 0)
                    self->fade_samples_remaining--;
            }
        }
        else if (self->is_fading_in)
        {
            if (self->fade_samples_remaining > 0)
            {
                self->fade_samples_remaining--;
                // Calculate fade in gain (0 -> 1) - Linear using reciprocal
                current_gain = 1.0f - (float)self->fade_samples_remaining * self->fade_sample_reciprocal;
                index_to_use = self->active_index; // Using the new index
            }
            else
            {
                // Fade in finished
                self->is_fading_in = false;
                current_gain = 1.0f;
                index_to_use = self->active_index;
            }
        }
        else
        { // Idle state
            current_gain = 1.0f;
            index_to_use = self->active_index;
        }

        /* --- Apply Switching / Fading --- */
        if (self->demux) /* 1 → N (Demux) - STILL NO FADE */
        {
            // NOTE: This uses self->active_index directly, not index_to_use, so it remains instant switch
            const int mode_l_param_idx = self->first_group_output_param + self->active_index * (self->lanes == 1 ? 2 : 4) + 1;
            const bool replace_l = self->v[mode_l_param_idx];
            float *dest_l = side_ptr[self->active_index][0];

            if (dest_l && inL)
            {
                dest_l[n] = replace_l ? inL[n] : dest_l[n] + inL[n];
            }

            if (self->lanes == 2)
            {
                const int mode_r_param_idx = self->first_group_output_param + self->active_index * 4 + 3;
                const bool replace_r = self->v[mode_r_param_idx];
                float *dest_r = side_ptr[self->active_index][1];
                if (dest_r && inR)
                {
                    dest_r[n] = replace_r ? inR[n] : dest_r[n] + inR[n];
                }
            }
        }
        else /* N → 1 (Mux) - APPLY FADE OUT/IN */
        {
            // Get input signal for the index being used in the current state
            float input_L = side_ptr[index_to_use][0] ? side_ptr[index_to_use][0][n] : 0.f;

            // Calculate output L
            if (outL)
            {
                outL[n] = input_L * current_gain;
            }

            if (self->lanes == 2)
            {
                float input_R = side_ptr[index_to_use][1] ? side_ptr[index_to_use][1][n] : 0.f;
                // Calculate output R
                if (outR)
                {
                    outR[n] = input_R * current_gain;
                }
            }
        }
    } // End audio sample loop
}

/* ───── factory & entry ───── */
// Make factory const
static const _NT_factory gFactory = {
    .guid = NT_MULTICHAR('S', 'S', 'W', '1'),
    .name = "Seq Switch Flex",
    .description = "Seq Switch (Mux/Demux, Clk, Rst). Mux: N->1 (Sig Out). Demux: 1->N (Sig In).",
    .numSpecifications = sizeof(gSpecs) / sizeof(gSpecs[0]),
    .specifications = gSpecs,
    .calculateStaticRequirements = nullptr,
    .initialise = nullptr,
    .calculateRequirements = calcReq,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = nullptr,
    .midiRealtime = nullptr,
    .midiMessage = nullptr,
    .tags = kNT_tagUtility,
    .hasCustomUi = nullptr, // Explicitly initialize omitted members
    .customUi = nullptr,
    .setupUi = nullptr};

extern "C" uintptr_t pluginEntry(_NT_selector s, uint32_t i)
{
    switch (s)
    {
    case kNT_selector_version:
        return kNT_apiVersionCurrent;
    case kNT_selector_numFactories:
        return 1;
    case kNT_selector_factoryInfo:
        return (i == 0) ? reinterpret_cast<uintptr_t>(&gFactory) : 0;
    default:
        return 0;
    }
}
