/* 12 x 16 matrix mixer for Expert Sleepers disting NT.
 *
 * UI: left encoder selects an input row, right encoder an output column.
 * Press either encoder to enter/leave level edit mode.  In edit mode either
 * encoder changes the selected crosspoint.  The centre pot always sets it.
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <distingnt/api.h>
#include <distingnt/serialisation.h>

namespace {

constexpr int kInputs = 12;
constexpr int kOutputs = 16;
constexpr int kCrosspoints = kInputs * kOutputs;
constexpr int kTargetFirstInputTrim = kCrosspoints;
constexpr int kTargetFirstOutputTrim = kTargetFirstInputTrim + kInputs;
constexpr int kTargets = kTargetFirstOutputTrim + kOutputs;
constexpr int kMidiLearnWords = (kTargets + 31) / 32;
constexpr int kParamFirstDirectOutput = 0;
constexpr int kParamFirstAuxOutput = 1;
constexpr int kParamFirstInputBus = 2;
constexpr int kParamFirstOutputBus = kParamFirstInputBus + kInputs;
constexpr int kParamFirstInputTrim = kParamFirstOutputBus + 2 * kOutputs;
constexpr int kParamFirstOutputTrim = kParamFirstInputTrim + kInputs;
constexpr int kParamRouteInput = kParamFirstOutputTrim + kOutputs;
constexpr int kParamRouteOutput = kParamRouteInput + 1;
constexpr int kParamRouteLevel = kParamRouteOutput + 1;
constexpr int kParamApplyRoute = kParamRouteLevel + 1;
constexpr int kParamStateRevision = kParamApplyRoute + 1;
constexpr int kParamInputCount = kParamStateRevision + 1;
constexpr int kParamDirectOutputCount = kParamInputCount + 1;
constexpr int kParamAuxOutputCount = kParamDirectOutputCount + 1;
constexpr int kNumParameters = kParamAuxOutputCount + 1;
constexpr int kMute = -600;          // -60.0 dB, displayed as -inf
constexpr int kMaxGain = 60;         // +6.0 dB
constexpr int16_t kNoMidiMapping = -1;
constexpr int16_t kNotMuted = -32768;
constexpr float kInputMeterScale = 0.1f; // NT audio busses are expressed in volts.
constexpr uint8_t kMidiClearedNoticeFrames = 45;

enum SelectionAxis : uint8_t {
    kSelectionSingle,
    kSelectionVertical,
    kSelectionHorizontal,
};

struct MatrixMixer : _NT_algorithm {
    int16_t gainValues[kCrosspoints];
    int16_t midiMappings[kTargets];
    int16_t muteRestoreValues[kTargets];
    uint32_t midiLearnTargets[kMidiLearnWords];
    float gains[kCrosspoints];
    float mixGains[kCrosspoints];
    float inputTrims[kInputs];
    float outputTrims[kOutputs];
    float inputMeters[kInputs];
    float outputMeters[kOutputs];
    float meterReleasePerFrame;
    uint16_t activeInputs[kOutputs];
    uint8_t row;       // 0..11 inputs, 12 output trims
    uint8_t column;    // 0 input trims, 1..16 outputs
    uint8_t selectionAxis;
    uint8_t selectionAnchorRow;
    uint8_t selectionAnchorColumn;
    uint8_t encoderPressRow[2];
    uint8_t encoderPressColumn[2];
    bool encoderPressUsed[2];
    bool editing;
    bool midiLearnArmed;
    bool hasMidiMappings;
    bool potCaught;
    bool potHasPrevious;
    bool potMoved;
    uint8_t midiClearedNoticeFrames;
    float potPrevious;
    float potArmPosition;
    uint32_t lastEncoderCycle[2];
};

static _NT_parameter parameters[kNumParameters];
static uint8_t inputTrimPageParams[kInputs];
static uint8_t outputPageParams[kOutputs];
static uint8_t inputRoutingPageParams[1 + kInputs];
static uint8_t outputRoutingPageParams[4 + 2 * kOutputs];
static const uint8_t routeEditPageParams[] = {
    kParamRouteInput, kParamRouteOutput, kParamRouteLevel, kParamApplyRoute
};
static _NT_parameterPage pages[5];
static _NT_parameterPages parameterPages;
static bool metadataReady = false;

inline int crosspointIndex(int input, int output) {
    return input * kOutputs + output;
}

inline int destinationParam(int output) {
    return kParamFirstOutputBus + 2 * output;
}

inline int outputModeParam(int output) {
    return destinationParam(output) + 1;
}

inline int inputBusParam(int input) {
    return kParamFirstInputBus + input;
}

inline int selectedTrimParam(const MatrixMixer* self) {
    if (self->row == kInputs)
        return kParamFirstOutputTrim + self->column - 1;
    return kParamFirstInputTrim + self->row;
}

inline int targetForCell(int row, int column) {
    if (row == kInputs)
        return kTargetFirstOutputTrim + column - 1;
    if (column == 0)
        return kTargetFirstInputTrim + row;
    return crosspointIndex(row, column - 1);
}

inline int selectedTarget(const MatrixMixer* self) {
    return targetForCell(self->row, self->column);
}

inline int targetValue(const MatrixMixer* self, int target) {
    if (target < kCrosspoints)
        return self->gainValues[target];
    if (target < kTargetFirstOutputTrim)
        return self->v[kParamFirstInputTrim + target - kTargetFirstInputTrim];
    return self->v[kParamFirstOutputTrim + target - kTargetFirstOutputTrim];
}

inline int selectedValue(const MatrixMixer* self) {
    return targetValue(self, selectedTarget(self));
}

inline int configuredInputs(const MatrixMixer* self) {
    const int value = self->v[kParamInputCount];
    return value < 1 ? 1 : (value > kInputs ? kInputs : value);
}

inline int configuredDirectOutputs(const MatrixMixer* self) {
    const int value = self->v[kParamDirectOutputCount];
    return value < 0 ? 0 : (value > 8 ? 8 : value);
}

inline int configuredAuxOutputs(const MatrixMixer* self) {
    const int value = self->v[kParamAuxOutputCount];
    return value < 0 ? 0 : (value > 8 ? 8 : value);
}

inline bool outputEnabled(const MatrixMixer* self, int output) {
    return output >= 0 && output < kOutputs &&
        (output < configuredDirectOutputs(self) ||
         (output >= 8 && output < 8 + configuredAuxOutputs(self)));
}

bool cellValid(const MatrixMixer* self, int row, int column) {
    if (row < 0 || row > kInputs || column < 0 || column > kOutputs)
        return false;
    if (row == kInputs)
        return column > 0 && outputEnabled(self, column - 1);
    if (row >= configuredInputs(self))
        return false;
    return column == 0 || outputEnabled(self, column - 1);
}

bool cellInSelection(const MatrixMixer* self, int row, int column) {
    if (!self->editing || !cellValid(self, row, column))
        return false;
    if (self->selectionAxis == kSelectionVertical) {
        const int first = self->selectionAnchorRow < self->row
            ? self->selectionAnchorRow : self->row;
        const int last = self->selectionAnchorRow > self->row
            ? self->selectionAnchorRow : self->row;
        return column == self->selectionAnchorColumn &&
               row >= first && row <= last;
    }
    if (self->selectionAxis == kSelectionHorizontal) {
        const int first = self->selectionAnchorColumn < self->column
            ? self->selectionAnchorColumn : self->column;
        const int last = self->selectionAnchorColumn > self->column
            ? self->selectionAnchorColumn : self->column;
        return row == self->selectionAnchorRow &&
               column >= first && column <= last;
    }
    return row == self->row && column == self->column;
}

bool cellIsOperationTarget(const MatrixMixer* self, int row, int column) {
    if (self->editing)
        return cellInSelection(self, row, column);
    return row == self->row && column == self->column;
}

int firstEnabledOutput(const MatrixMixer* self) {
    for (int output = 0; output < kOutputs; ++output) {
        if (outputEnabled(self, output))
            return output;
    }
    return -1;
}

void normalizeSelection(MatrixMixer* self) {
    const int inputs = configuredInputs(self);
    if (self->row > kInputs)
        self->row = 0;
    if (self->column > kOutputs)
        self->column = 0;
    if (self->row < kInputs && self->row >= inputs)
        self->row = static_cast<uint8_t>(inputs - 1);
    if (self->row == kInputs) {
        if (self->column == 0 || !outputEnabled(self, self->column - 1)) {
            const int output = firstEnabledOutput(self);
            if (output >= 0)
                self->column = static_cast<uint8_t>(output + 1);
            else {
                self->row = static_cast<uint8_t>(inputs - 1);
                self->column = 0;
            }
        }
    } else if (self->column > 0 && !outputEnabled(self, self->column - 1)) {
        self->column = 0;
    }
    if (!cellValid(self, self->selectionAnchorRow,
                   self->selectionAnchorColumn)) {
        self->selectionAxis = kSelectionSingle;
        self->selectionAnchorRow = self->row;
        self->selectionAnchorColumn = self->column;
    }
}

uint8_t moveRow(const MatrixMixer* self, uint8_t row, int direction) {
    const int inputs = configuredInputs(self);
    int candidate = row;
    for (int attempt = 0; attempt <= kInputs; ++attempt) {
        candidate += direction;
        if (candidate < 0) candidate = kInputs;
        if (candidate > kInputs) candidate = 0;
        if (candidate < inputs)
            return static_cast<uint8_t>(candidate);
        if (candidate == kInputs && self->column > 0 &&
            outputEnabled(self, self->column - 1))
            return static_cast<uint8_t>(candidate);
    }
    return row;
}

uint8_t moveColumn(const MatrixMixer* self, uint8_t column, int direction) {
    int candidate = column;
    for (int attempt = 0; attempt <= kOutputs; ++attempt) {
        candidate += direction;
        if (candidate < 0) candidate = kOutputs;
        if (candidate > kOutputs) candidate = 0;
        if (candidate == 0) {
            if (self->row < configuredInputs(self))
                return 0;
        } else if (outputEnabled(self, candidate - 1)) {
            return static_cast<uint8_t>(candidate);
        }
    }
    return column;
}

void prepareMetadata() {
    if (metadataReady)
        return;

    parameters[kParamFirstDirectOutput] = {
        "First direct output", 1,
        static_cast<int16_t>(kNT_lastBus - 7),
        13, kNT_unitNone, 0, nullptr
    };
    parameters[kParamFirstAuxOutput] = {
        "First aux output", 1,
        static_cast<int16_t>(kNT_lastBus - 7),
        21, kNT_unitNone, 0, nullptr
    };
    inputRoutingPageParams[0] = kParamInputCount;
    for (int in = 0; in < kInputs; ++in) {
        const int p = inputBusParam(in);
        parameters[p] = { "Source", 1, static_cast<int16_t>(kNT_lastBus),
                          static_cast<int16_t>(in + 1),
                          kNT_unitAudioInput, 0, nullptr };
        inputRoutingPageParams[in + 1] = static_cast<uint8_t>(p);
    }

    int routing = 0;
    outputRoutingPageParams[routing++] = kParamDirectOutputCount;
    outputRoutingPageParams[routing++] = kParamFirstDirectOutput;
    for (int out = 0; out < kOutputs; ++out) {
        if (out == 8) {
            outputRoutingPageParams[routing++] = kParamAuxOutputCount;
            outputRoutingPageParams[routing++] = kParamFirstAuxOutput;
        }
        const int destination = destinationParam(out);
        const int mode = outputModeParam(out);
        parameters[destination] = {
            "Destination", 1, static_cast<int16_t>(kNT_lastBus),
            static_cast<int16_t>(out < 8 ? 13 + out : 21 + out - 8),
            kNT_unitAudioOutput, 0, nullptr
        };
        parameters[mode] = {
            "Mode", 0, 1, 0, kNT_unitOutputMode, 0, nullptr
        };
        outputRoutingPageParams[routing++] = static_cast<uint8_t>(destination);
        outputRoutingPageParams[routing++] = static_cast<uint8_t>(mode);
    }

    for (int in = 0; in < kInputs; ++in) {
        const int trim = kParamFirstInputTrim + in;
        parameters[trim] = { "Input trim", kMute, kMaxGain, 0,
                             kNT_unitDb_minInf, kNT_scaling10, nullptr };
        inputTrimPageParams[in] = static_cast<uint8_t>(trim);
    }
    for (int out = 0; out < kOutputs; ++out) {
        const int trim = kParamFirstOutputTrim + out;
        parameters[trim] = { "Output trim", kMute, kMaxGain, 0,
                             kNT_unitDb_minInf, kNT_scaling10, nullptr };
        outputPageParams[out] = static_cast<uint8_t>(trim);
    }
    parameters[kParamRouteInput] = {
        "Route input", 1, kInputs, 1, kNT_unitNone, 0, nullptr
    };
    parameters[kParamRouteOutput] = {
        "Route output", 1, kOutputs, 1, kNT_unitNone, 0, nullptr
    };
    parameters[kParamRouteLevel] = {
        "Route level", kMute, kMaxGain, kMute,
        kNT_unitDb_minInf, kNT_scaling10, nullptr
    };
    parameters[kParamApplyRoute] = {
        "Apply route", 0, 1, 0, kNT_unitNone, 0, nullptr
    };
    parameters[kParamStateRevision] = {
        "Matrix state", 0, 32767, 0, kNT_unitNone, 0, nullptr
    };
    parameters[kParamInputCount] = {
        "Internal inputs", 1, kInputs, kInputs, kNT_unitNone, 0, nullptr
    };
    parameters[kParamDirectOutputCount] = {
        "Internal outputs", 0, 8, 8, kNT_unitNone, 0, nullptr
    };
    parameters[kParamAuxOutputCount] = {
        "Expander outputs", 0, 8, 8, kNT_unitNone, 0, nullptr
    };
    pages[0].name = "Input trims";
    pages[0].numParams = kInputs;
    pages[0].params = inputTrimPageParams;
    pages[1].name = "Output trims";
    pages[1].numParams = kOutputs;
    pages[1].params = outputPageParams;
    pages[2].name = "Input routing";
    pages[2].numParams = ARRAY_SIZE(inputRoutingPageParams);
    pages[2].params = inputRoutingPageParams;
    pages[3].name = "Route edit";
    pages[3].numParams = ARRAY_SIZE(routeEditPageParams);
    pages[3].params = routeEditPageParams;
    pages[4].name = "Output routing";
    pages[4].numParams = ARRAY_SIZE(outputRoutingPageParams);
    pages[4].params = outputRoutingPageParams;
    parameterPages.numPages = ARRAY_SIZE(pages);
    parameterPages.pages = pages;
    metadataReady = true;
}

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    prepareMetadata();
    req.numParameters = ARRAY_SIZE(parameters);
    req.sram = sizeof(MatrixMixer);
    req.dram = req.dtc = req.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs,
                         const _NT_algorithmRequirements&, const int32_t*) {
    MatrixMixer* self = new (ptrs.sram) MatrixMixer();
    self->parameters = parameters;
    self->parameterPages = &parameterPages;
    self->row = 0;
    self->column = 1;
    self->selectionAxis = kSelectionSingle;
    self->selectionAnchorRow = self->row;
    self->selectionAnchorColumn = self->column;
    self->encoderPressRow[0] = self->encoderPressRow[1] = self->row;
    self->encoderPressColumn[0] = self->encoderPressColumn[1] = self->column;
    self->encoderPressUsed[0] = self->encoderPressUsed[1] = false;
    self->editing = false;
    self->midiLearnArmed = false;
    self->hasMidiMappings = false;
    self->potCaught = false;
    self->potHasPrevious = false;
    self->potMoved = false;
    self->midiClearedNoticeFrames = 0;
    self->potPrevious = 0.0f;
    self->potArmPosition = 0.0f;
    self->lastEncoderCycle[0] = self->lastEncoderCycle[1] = 0;
    self->meterReleasePerFrame = NT_globals.sampleRate
        ? 1.0f / (0.15f * NT_globals.sampleRate) : 0.0f;
    for (int16_t& value : self->gainValues)
        value = kMute;
    for (int16_t& mapping : self->midiMappings)
        mapping = kNoMidiMapping;
    for (int16_t& restore : self->muteRestoreValues)
        restore = kNotMuted;
    for (uint32_t& targets : self->midiLearnTargets)
        targets = 0;
    for (float& gain : self->gains)
        gain = 0.0f;
    for (float& gain : self->mixGains)
        gain = 0.0f;
    for (int i = 0; i < kInputs; ++i) {
        self->inputTrims[i] = 1.0f;
        self->inputMeters[i] = 0.0f;
    }
    for (int i = 0; i < kOutputs; ++i) {
        self->outputTrims[i] = 1.0f;
        self->outputMeters[i] = 0.0f;
        self->activeInputs[i] = 0;
    }
    return self;
}

inline float dbToGain(int value) {
    return value <= kMute ? 0.0f : std::pow(10.0f, value / 200.0f);
}

inline void refreshCrosspoint(MatrixMixer* self, int input, int output) {
    const int crosspoint = input * kOutputs + output;
    self->mixGains[crosspoint] = self->gains[crosspoint] *
        self->inputTrims[input] * self->outputTrims[output];
    const uint16_t inputBit = static_cast<uint16_t>(1u << input);
    if (self->mixGains[crosspoint] != 0.0f)
        self->activeInputs[output] |= inputBit;
    else
        self->activeInputs[output] &= static_cast<uint16_t>(~inputBit);
}

void refreshInputRow(MatrixMixer* self, int input) {
    for (int output = 0; output < kOutputs; ++output)
        refreshCrosspoint(self, input, output);
}

void refreshOutputColumn(MatrixMixer* self, int output) {
    for (int input = 0; input < kInputs; ++input)
        refreshCrosspoint(self, input, output);
}

void setCrosspointValue(MatrixMixer* self, int crosspoint, int value) {
    if (value < kMute) value = kMute;
    if (value > kMaxGain) value = kMaxGain;
    self->gainValues[crosspoint] = static_cast<int16_t>(value);
    self->gains[crosspoint] = dbToGain(value);
    refreshCrosspoint(self, crosspoint / kOutputs, crosspoint % kOutputs);
}

void markMatrixDirty(MatrixMixer* self, bool fromUi) {
    const int revision = self->v[kParamStateRevision] >= 32767
        ? 0 : self->v[kParamStateRevision] + 1;
    const uint32_t parameter = kParamStateRevision + NT_parameterOffset();
    const int32_t algorithmIndex = NT_algorithmIndex(self);
    if (algorithmIndex < 0)
        return;
    if (fromUi)
        NT_setParameterFromUi(static_cast<uint32_t>(algorithmIndex),
                              parameter, revision);
    else
        NT_setParameterFromAudio(static_cast<uint32_t>(algorithmIndex),
                                 parameter, revision);
}

void parameterChanged(_NT_algorithm* algorithm, int p) {
    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    if (p == kParamInputCount || p == kParamDirectOutputCount ||
        p == kParamAuxOutputCount) {
        normalizeSelection(self);
        return;
    }
    if (p == kParamFirstDirectOutput || p == kParamFirstAuxOutput) {
        const int firstDestination = p == kParamFirstDirectOutput ? 0 : 8;
        const int base = self->v[p];
        if (base < 1 || base > kNT_lastBus - 7)
            return;
        const int32_t algorithmIndex = NT_algorithmIndex(self);
        if (algorithmIndex >= 0) {
            for (int i = 0; i < 8; ++i)
                NT_setParameterFromAudio(static_cast<uint32_t>(algorithmIndex),
                    destinationParam(firstDestination + i) + NT_parameterOffset(),
                    base + i);
        }
        return;
    }
    if (p == kParamApplyRoute && self->v[p]) {
        const int input = self->v[kParamRouteInput] - 1;
        const int output = self->v[kParamRouteOutput] - 1;
        if (input >= 0 && input < kInputs && output >= 0 && output < kOutputs) {
            const int crosspoint = crosspointIndex(input, output);
            self->muteRestoreValues[crosspoint] = kNotMuted;
            setCrosspointValue(self, crosspoint, self->v[kParamRouteLevel]);
            markMatrixDirty(self, false);
        }
        const int32_t algorithmIndex = NT_algorithmIndex(self);
        if (algorithmIndex >= 0)
            NT_setParameterFromAudio(static_cast<uint32_t>(algorithmIndex),
                                     kParamApplyRoute + NT_parameterOffset(), 0);
        return;
    }
    if (p >= kParamFirstInputTrim && p < kParamFirstOutputTrim) {
        const int input = p - kParamFirstInputTrim;
        self->inputTrims[input] = dbToGain(self->v[p]);
        refreshInputRow(self, input);
    } else if (p >= kParamFirstOutputTrim && p < kParamRouteInput) {
        const int output = p - kParamFirstOutputTrim;
        self->outputTrims[output] = dbToGain(self->v[p]);
        refreshOutputColumn(self, output);
    }
}

void step(_NT_algorithm* algorithm, float* busFrames, int numFramesBy4) {
    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    const int frames = numFramesBy4 * 4;

    const float* sources[kInputs] = {};
    float inputPeak[kInputs] = {};
    float outputPeak[kOutputs] = {};
    const int inputCount = configuredInputs(self);
    for (int input = 0; input < inputCount; ++input) {
        const int bus = self->v[inputBusParam(input)];
        if (bus < 1 || bus > kNT_lastBus)
            continue;
        sources[input] = busFrames + (bus - 1) * frames;
        for (int frame = 0; frame < frames; frame += 4) {
            const float sample = sources[input][frame];
            const float magnitude = (sample < 0.0f ? -sample : sample) *
                                    kInputMeterScale;
            if (magnitude > inputPeak[input]) inputPeak[input] = magnitude;
        }
    }

    for (int output = 0; output < kOutputs; ++output) {
        if (!outputEnabled(self, output))
            continue;
        const int bus = self->v[destinationParam(output)];
        if (bus < 1 || bus > kNT_lastBus)
            continue;
        float* destination = busFrames + (bus - 1) * frames;
        const bool replace = self->v[outputModeParam(output)] != 0;
        uint16_t active = self->activeInputs[output];
        for (int input = 0; input < kInputs; ++input) {
            if (!sources[input])
                active &= static_cast<uint16_t>(~(1u << input));
        }

        if (!active) {
            if (replace)
                std::memset(destination, 0,
                            static_cast<size_t>(frames) * sizeof(float));
            else {
                for (int frame = 0; frame < frames; frame += 4) {
                    const float sample = destination[frame];
                    const float magnitude = sample < 0.0f ? -sample : sample;
                    if (magnitude > outputPeak[output])
                        outputPeak[output] = magnitude;
                }
            }
            continue;
        }

        for (int frame = 0; frame < frames; ++frame) {
            float mixed = 0.0f;
            uint16_t remaining = active;
            while (remaining) {
                const int input = __builtin_ctz(static_cast<unsigned>(remaining));
                const float gain = self->mixGains[input * kOutputs + output];
                mixed += sources[input][frame] * gain;
                remaining &= static_cast<uint16_t>(remaining - 1);
            }
            if (replace)
                destination[frame] = mixed;
            else
                destination[frame] += mixed;
            const float sample = destination[frame];
            const float magnitude = sample < 0.0f ? -sample : sample;
            if (magnitude > outputPeak[output]) outputPeak[output] = magnitude;
        }
    }

    const float release = frames * self->meterReleasePerFrame;
    for (int input = 0; input < kInputs; ++input) {
        float decayed = self->inputMeters[input] - release;
        if (decayed < 0.0f) decayed = 0.0f;
        self->inputMeters[input] = inputPeak[input] > decayed
            ? inputPeak[input] : decayed;
    }
    for (int output = 0; output < kOutputs; ++output) {
        float decayed = self->outputMeters[output] - release;
        if (decayed < 0.0f) decayed = 0.0f;
        self->outputMeters[output] = outputPeak[output] > decayed
            ? outputPeak[output] : decayed;
    }
}

int intensityFor(int value) {
    if (value <= kMute)
        return 1;
    // Perceptual-ish display range: -60 dB..+6 dB -> shades 2..13.
    return 2 + (value - kMute) * 11 / (kMaxGain - kMute);
}

int levelBlockCount(int value) {
    if (value <= kMute)
        return 0;
    // Eleven 6 dB blocks: ten reach unity, the last is +0.1..+6.0 dB.
    int count = (value - kMute + 59) / 60;
    return count > 11 ? 11 : count;
}

int meterIntensity(float peak) {
    if (peak <= 0.0001f) return 1;
    const float db = 20.0f * std::log10(peak);
    if (db >= 0.0f) return 15;
    return 3 + static_cast<int>((db + 48.0f) * 11.0f / 48.0f);
}

void formatDb(char* text, int size, int value) {
    (void)size;
    if (value <= kMute) {
        std::memcpy(text, "-inf", 5);
        return;
    }
    const int magnitude = value < 0 ? -value : value;
    int pos = 0;
    text[pos++] = value < 0 ? '-' : '+';
    const int whole = magnitude / 10;
    if (whole >= 10) text[pos++] = static_cast<char>('0' + whole / 10);
    text[pos++] = static_cast<char>('0' + whole % 10);
    text[pos++] = '.';
    text[pos++] = static_cast<char>('0' + magnitude % 10);
    text[pos] = '\0';
}

void appendTwoDigits(char* text, int& pos, int value) {
    text[pos++] = static_cast<char>('0' + (value / 10) % 10);
    text[pos++] = static_cast<char>('0' + value % 10);
}

void formatRoute(char* text, const MatrixMixer* self) {
    int pos = 0;
    if (self->row == kInputs) {
        const char prefix[] = "OUTPUT ";
        std::memcpy(text + pos, prefix, sizeof(prefix) - 1); pos += sizeof(prefix) - 1;
        appendTwoDigits(text, pos, self->column);
        const char suffix[] = " TRIM";
        std::memcpy(text + pos, suffix, sizeof(suffix) - 1); pos += sizeof(suffix) - 1;
    } else if (self->column == 0) {
        const char prefix[] = "INPUT ";
        std::memcpy(text + pos, prefix, sizeof(prefix) - 1); pos += sizeof(prefix) - 1;
        appendTwoDigits(text, pos, self->row + 1);
        const char suffix[] = " TRIM";
        std::memcpy(text + pos, suffix, sizeof(suffix) - 1); pos += sizeof(suffix) - 1;
    } else {
        const char prefix[] = "IN ";
        std::memcpy(text + pos, prefix, sizeof(prefix) - 1); pos += sizeof(prefix) - 1;
        appendTwoDigits(text, pos, self->row + 1);
        const char middle[] = "  >  OUT ";
        std::memcpy(text + pos, middle, sizeof(middle) - 1); pos += sizeof(middle) - 1;
        appendTwoDigits(text, pos, self->column);
    }
    text[pos] = '\0';
}

bool draw(_NT_algorithm* algorithm) {
    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    normalizeSelection(self);
    std::memset(NT_screen, 0, sizeof(NT_screen));
    char route[24];
    char level[16];
    const int value = selectedValue(self);
    formatRoute(route, self);
    formatDb(level, sizeof(level), value);

    constexpr int x0 = 2;
    constexpr int y0 = 9;
    constexpr int cellW = 7;
    constexpr int cellH = 4;
    const int inputCount = configuredInputs(self);
    if (self->midiClearedNoticeFrames) {
        NT_drawText(2, 1, "MIDI CLEARED", 15, kNT_textLeft, kNT_textTiny);
        --self->midiClearedNoticeFrames;
    } else {
        NT_drawText(2, 1, "12x16 MATRIX", 8, kNT_textLeft, kNT_textTiny);
    }
    for (int in = 0; in < kInputs; ++in) {
        for (int out = 0; out < kOutputs; ++out) {
            const int x = x0 + (out + 1) * cellW;
            const int y = y0 + in * cellH;
            if (in >= inputCount || !outputEnabled(self, out))
                continue;
            const int shade = intensityFor(
                self->gainValues[crosspointIndex(in, out)]);
            NT_drawShapeI(kNT_rectangle, x + 2, y + 1, x + 4, y + 2, shade);
            if (cellInSelection(self, in, out + 1))
                NT_drawShapeI(kNT_box, x, y, x + 6, y + 3,
                              in == self->row && out + 1 == self->column ? 15 : 9);
            else if (in == self->row && out + 1 == self->column)
                NT_drawShapeI(kNT_box, x, y, x + 6, y + 3, 11);
        }
        // Leftmost column: input trim, with live input meter as its centre pixel.
        const int x = x0;
        const int y = y0 + in * cellH;
        if (in >= inputCount)
            continue;
        NT_drawShapeI(kNT_rectangle, x + 2, y + 1, x + 4, y + 2,
                      intensityFor(self->v[kParamFirstInputTrim + in]));
        NT_drawShapeI(kNT_point, x + 3, y + 1, x + 3, y + 1,
                      meterIntensity(self->inputMeters[in]));
        if (cellInSelection(self, in, 0))
            NT_drawShapeI(kNT_box, x, y, x + 6, y + 3,
                          in == self->row && self->column == 0 ? 15 : 9);
        else if (in == self->row && self->column == 0)
            NT_drawShapeI(kNT_box, x, y, x + 6, y + 3, 11);
    }
    // Bottom row: output trims with live output meters.
    for (int out = 0; out < kOutputs; ++out) {
        const int x = x0 + (out + 1) * cellW;
        const int y = y0 + kInputs * cellH;
        if (!outputEnabled(self, out))
            continue;
        NT_drawShapeI(kNT_rectangle, x + 2, y + 1, x + 4, y + 2,
                      intensityFor(self->v[kParamFirstOutputTrim + out]));
        NT_drawShapeI(kNT_point, x + 3, y + 1, x + 3, y + 1,
                      meterIntensity(self->outputMeters[out]));
        if (cellInSelection(self, kInputs, out + 1))
            NT_drawShapeI(kNT_box, x, y, x + 6, y + 3,
                          self->row == kInputs && out + 1 == self->column ? 15 : 9);
        else if (self->row == kInputs && out + 1 == self->column)
            NT_drawShapeI(kNT_box, x, y, x + 6, y + 3, 11);
    }

    // Internal outputs 1-8 | expander outputs 1-8.
    NT_drawShapeI(kNT_line, 64, y0 - 1, 64, y0 + kInputs * cellH + 3, 6);

    NT_drawShapeI(kNT_line, 124, 3, 124, 61, 4);

    // Selected crosspoint inspector: deliberately compact, leaving the lower
    // half permanently available for the complete input/output meter bridge.
    NT_drawText(190, 8, route, 11, kNT_textCentre, kNT_textTiny);
    const int activeBlocks = levelBlockCount(value);
    for (int block = 0; block < 11; ++block) {
        const int x = 130 + block * 4;
        const int shade = block < activeBlocks ? 4 + block : 2;
        NT_drawShapeI(kNT_rectangle, x, 20, x + 2, 25, shade);
    }
    NT_drawText(190, 25, level, 15, kNT_textCentre, kNT_textNormal);
    NT_drawText(218, 25, "dB", 7, kNT_textLeft, kNT_textTiny);
    if (self->midiLearnArmed)
        NT_drawText(253, 25, "LEARN", 15, kNT_textRight, kNT_textTiny);
    else if (self->editing)
        NT_drawText(253, 25, "SEL", 15, kNT_textRight, kNT_textTiny);

    // Always-visible meter bridge. Each channel gets a 2 px bar plus 2 px
    // spacing: 12 inputs on the left, 16 outputs on the right.
    constexpr int meterTop = 38;
    constexpr int meterBottom = 60;
    NT_drawText(130, 31, "INPUTS", 6, kNT_textLeft, kNT_textTiny);
    NT_drawText(190, 31, "OUTPUTS", 6, kNT_textLeft, kNT_textTiny);
    NT_drawShapeI(kNT_line, 184, 32, 184, 62, 3);
    for (int in = 0; in < kInputs; ++in) {
        const int x = 130 + in * 4;
        int height = 0;
        if (self->inputMeters[in] > 0.0001f) {
            float db = 20.0f * std::log10(self->inputMeters[in]);
            if (db < -48.0f) db = -48.0f;
            if (db > 0.0f) db = 0.0f;
            height = 1 + static_cast<int>((db + 48.0f) *
                                          (meterBottom - meterTop - 1) / 48.0f);
        }
        NT_drawShapeI(kNT_line, x, meterTop, x, meterBottom,
                      in < inputCount ? 2 : 1);
        if (height && in < inputCount)
            NT_drawShapeI(kNT_rectangle, x, meterBottom - height, x + 1, meterBottom,
                          in == self->row ? 15 : 10);
    }
    for (int out = 0; out < kOutputs; ++out) {
        const int x = 190 + out * 4;
        int height = 0;
        if (self->outputMeters[out] > 0.0001f) {
            float db = 20.0f * std::log10(self->outputMeters[out]);
            if (db < -48.0f) db = -48.0f;
            if (db > 0.0f) db = 0.0f;
            height = 1 + static_cast<int>((db + 48.0f) *
                                          (meterBottom - meterTop - 1) / 48.0f);
        }
        NT_drawShapeI(kNT_line, x, meterTop, x, meterBottom,
                      outputEnabled(self, out) ? 2 : 1);
        if (height && outputEnabled(self, out))
            NT_drawShapeI(kNT_rectangle, x, meterBottom - height, x + 1, meterBottom,
                          out + 1 == self->column ? 15 : 10);
    }
    NT_drawShapeI(kNT_line, 220, meterTop - 2, 220, meterBottom + 1, 5);
    return true;
}

bool setTargetValue(MatrixMixer* self, int target, int value,
                    bool fromUi, bool clearMuteMemory) {
    if (value < kMute) value = kMute;
    if (value > kMaxGain) value = kMaxGain;
    bool changed = false;
    if (clearMuteMemory && self->muteRestoreValues[target] != kNotMuted) {
        self->muteRestoreValues[target] = kNotMuted;
        changed = true;
    }
    if (targetValue(self, target) == value)
        return changed;
    if (target < kCrosspoints) {
        setCrosspointValue(self, target, value);
    } else {
        const int parameter = target < kTargetFirstOutputTrim
            ? kParamFirstInputTrim + target - kTargetFirstInputTrim
            : kParamFirstOutputTrim + target - kTargetFirstOutputTrim;
        const uint32_t algorithmIndex = static_cast<uint32_t>(NT_algorithmIndex(self));
        if (fromUi)
            NT_setParameterFromUi(algorithmIndex,
                                  parameter + NT_parameterOffset(), value);
        else
            NT_setParameterFromAudio(algorithmIndex,
                                     parameter + NT_parameterOffset(), value);
    }
    return true;
}

template <typename Operation>
void forEachOperationTarget(MatrixMixer* self, Operation operation) {
    for (int row = 0; row <= kInputs; ++row) {
        for (int column = 0; column <= kOutputs; ++column) {
            if (cellValid(self, row, column) &&
                cellIsOperationTarget(self, row, column))
                operation(targetForCell(row, column));
        }
    }
}

void cancelMidiLearn(MatrixMixer* self) {
    self->midiLearnArmed = false;
    for (uint32_t& targets : self->midiLearnTargets)
        targets = 0;
}

bool midiLearnIncludes(const MatrixMixer* self, int target) {
    return target >= 0 && target < kTargets &&
        (self->midiLearnTargets[target / 32] &
         (static_cast<uint32_t>(1) << (target % 32))) != 0;
}

void armMidiLearnForSelection(MatrixMixer* self) {
    cancelMidiLearn(self);
    forEachOperationTarget(self, [&](int target) {
        self->midiLearnTargets[target / 32] |=
            static_cast<uint32_t>(1) << (target % 32);
        self->midiLearnArmed = true;
    });
}

void clearMidiMappingsForSelection(MatrixMixer* self) {
    cancelMidiLearn(self);
    bool changed = false;
    forEachOperationTarget(self, [&](int target) {
        if (self->midiMappings[target] != kNoMidiMapping) {
            self->midiMappings[target] = kNoMidiMapping;
            changed = true;
        }
    });

    self->hasMidiMappings = false;
    for (int target = 0; target < kTargets; ++target) {
        if (self->midiMappings[target] != kNoMidiMapping) {
            self->hasMidiMappings = true;
            break;
        }
    }
    self->midiClearedNoticeFrames = kMidiClearedNoticeFrames;
    if (changed)
        markMatrixDirty(self, true);
}

void setSelectionGain(MatrixMixer* self, int value, bool fromUi) {
    bool changed = false;
    forEachOperationTarget(self, [&](int target) {
        changed |= setTargetValue(self, target, value, fromUi, true);
    });
    if (changed)
        markMatrixDirty(self, fromUi);
}

void toggleSelectionMute(MatrixMixer* self) {
    bool allMuted = true;
    forEachOperationTarget(self, [&](int target) {
        if (self->muteRestoreValues[target] == kNotMuted)
            allMuted = false;
    });

    bool changed = false;
    forEachOperationTarget(self, [&](int target) {
        if (allMuted) {
            const int restore = self->muteRestoreValues[target];
            if (restore != kNotMuted) {
                self->muteRestoreValues[target] = kNotMuted;
                changed |= setTargetValue(self, target, restore, true, false);
                changed = true;
            }
        } else if (self->muteRestoreValues[target] == kNotMuted) {
            self->muteRestoreValues[target] =
                static_cast<int16_t>(targetValue(self, target));
            changed |= setTargetValue(self, target, kMute, true, false);
            changed = true;
        }
    });
    if (changed)
        markMatrixDirty(self, true);
}

int acceleratedEncoderStep(MatrixMixer* self, int encoder, int delta) {
    if (!delta) return 0;
    const uint32_t now = NT_getCpuCycleCount();
    const uint32_t elapsed = now - self->lastEncoderCycle[encoder];
    self->lastEncoderCycle[encoder] = now;
    // Cortex-M7 runs at 600 MHz. Slow turns retain the parameter's minimum
    // 0.1 dB resolution; only closely spaced detents accelerate.
    int scale = 1;
    if (elapsed < 7200000u) scale = 20;       // under 12 ms: 2.0 dB
    else if (elapsed < 18000000u) scale = 5; // under 30 ms: 0.5 dB
    return delta * scale;
}

int snapThroughUnity(int current, int next) {
    if ((current < 0 && next > 0) || (current > 0 && next < 0)) return 0;
    return next;
}

void adjustSelectionGain(MatrixMixer* self, int delta) {
    bool changed = false;
    forEachOperationTarget(self, [&](int target) {
        const int current = targetValue(self, target);
        const int next = snapThroughUnity(current, current + delta);
        changed |= setTargetValue(self, target, next, true, true);
    });
    if (changed)
        markMatrixDirty(self, true);
}

int midiValueToGain(uint8_t value) {
    int gain = kMute + (static_cast<int>(value) * (kMaxGain - kMute) + 63) / 127;
    if (gain > -5 && gain < 5)
        gain = 0;
    return gain;
}

void midiMessage(_NT_algorithm* algorithm, uint8_t byte0,
                 uint8_t byte1, uint8_t byte2) {
    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    if ((byte0 & 0xf0) != 0xb0)
        return;
    const int16_t mapping = static_cast<int16_t>(
        ((byte0 & 0x0f) << 7) | (byte1 & 0x7f));
    bool changed = false;
    if (self->midiLearnArmed) {
        for (int target = 0; target < kTargets; ++target) {
            if (midiLearnIncludes(self, target)) {
                self->midiMappings[target] = mapping;
                self->hasMidiMappings = true;
                changed = true;
            }
        }
        cancelMidiLearn(self);
    }
    if (!self->hasMidiMappings)
        return;
    const int gain = midiValueToGain(byte2 & 0x7f);
    for (int target = 0; target < kTargets; ++target) {
        if (self->midiMappings[target] == mapping)
            changed |= setTargetValue(self, target, gain, false, true);
    }
    if (changed)
        markMatrixDirty(self, false);
}

uint32_t hasCustomUi(_NT_algorithm*) {
    return kNT_encoderL | kNT_encoderR | kNT_encoderButtonL |
           kNT_encoderButtonR | kNT_potButtonL | kNT_potButtonC |
           kNT_potButtonR | kNT_potC;
}

void armPotPickup(MatrixMixer* self, float position) {
    self->potCaught = false;
    self->potHasPrevious = true;
    self->potMoved = false;
    self->potPrevious = position;
    self->potArmPosition = position;
}

void customUi(_NT_algorithm* algorithm, const _NT_uiData& data) {
    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    normalizeSelection(self);
    const uint16_t pressed = data.controls & ~data.lastButtons;
    const uint16_t released = data.lastButtons & ~data.controls;
    constexpr uint16_t encoderButtons[2] = {
        kNT_encoderButtonL, kNT_encoderButtonR
    };

    for (int encoder = 0; encoder < 2; ++encoder) {
        if (pressed & encoderButtons[encoder]) {
            self->encoderPressUsed[encoder] = false;
            self->encoderPressRow[encoder] = self->row;
            self->encoderPressColumn[encoder] = self->column;
        }
    }

    if (pressed & kNT_potButtonL)
        toggleSelectionMute(self);
    if (pressed & kNT_potButtonC)
        clearMidiMappingsForSelection(self);
    if (pressed & kNT_potButtonR) {
        if (self->midiLearnArmed)
            cancelMidiLearn(self);
        else
            armMidiLearnForSelection(self);
    }

    bool consumedEncoder[2] = { false, false };
    for (int encoder = 0; encoder < 2; ++encoder) {
        if (!data.encoders[encoder] ||
            !(data.controls & encoderButtons[encoder]))
            continue;
        consumedEncoder[encoder] = true;
        if (!self->encoderPressUsed[encoder]) {
            self->encoderPressUsed[encoder] = true;
            self->editing = true;
            self->selectionAxis = encoder == 0
                ? kSelectionVertical : kSelectionHorizontal;
            self->selectionAnchorRow = self->encoderPressRow[encoder];
            self->selectionAnchorColumn = self->encoderPressColumn[encoder];
            cancelMidiLearn(self);
        }

        const int direction = data.encoders[encoder] < 0 ? -1 : 1;
        int steps = data.encoders[encoder] < 0
            ? -data.encoders[encoder] : data.encoders[encoder];
        while (steps--) {
            if (encoder == 0) {
                const uint8_t next = moveRow(self, self->row, direction);
                if ((direction > 0 && next <= self->row) ||
                    (direction < 0 && next >= self->row))
                    break;
                self->row = next;
            } else {
                const uint8_t next = moveColumn(self, self->column, direction);
                if ((direction > 0 && next <= self->column) ||
                    (direction < 0 && next >= self->column))
                    break;
                self->column = next;
            }
        }
        armPotPickup(self, data.pots[1]);
    }

    for (int encoder = 0; encoder < 2; ++encoder) {
        if (!(released & encoderButtons[encoder]))
            continue;
        if (!self->encoderPressUsed[encoder]) {
            self->editing = !self->editing;
            self->selectionAxis = kSelectionSingle;
            self->selectionAnchorRow = self->row;
            self->selectionAnchorColumn = self->column;
            cancelMidiLearn(self);
        }
        self->encoderPressUsed[encoder] = false;
    }

    if (self->editing) {
        const int delta =
            (consumedEncoder[0] ? 0 : acceleratedEncoderStep(self, 0, data.encoders[0])) +
            (consumedEncoder[1] ? 0 : acceleratedEncoderStep(self, 1, data.encoders[1]));
        if (delta) {
            adjustSelectionGain(self, delta);
            armPotPickup(self, data.pots[1]);
        }
    } else {
        const uint8_t oldRow = self->row;
        const uint8_t oldColumn = self->column;
        if (data.encoders[0] && !consumedEncoder[0]) {
            const int direction = data.encoders[0] < 0 ? -1 : 1;
            int steps = data.encoders[0] < 0 ? -data.encoders[0] : data.encoders[0];
            while (steps--)
                self->row = moveRow(self, self->row, direction);
        }
        if (data.encoders[1] && !consumedEncoder[1]) {
            const int direction = data.encoders[1] < 0 ? -1 : 1;
            int steps = data.encoders[1] < 0 ? -data.encoders[1] : data.encoders[1];
            while (steps--)
                self->column = moveColumn(self, self->column, direction);
        }
        if (self->row != oldRow || self->column != oldColumn) {
            self->selectionAxis = kSelectionSingle;
            self->selectionAnchorRow = self->row;
            self->selectionAnchorColumn = self->column;
            cancelMidiLearn(self);
            armPotPickup(self, data.pots[1]);
        }
    }
    if (data.controls & kNT_potC) {
        const float position = data.pots[1];
        const float target = (selectedValue(self) - kMute) /
                             static_cast<float>(kMaxGain - kMute);
        if (!self->potHasPrevious) {
            self->potPrevious = position;
            self->potArmPosition = position;
            self->potHasPrevious = true;
        } else if (!self->potCaught) {
            const float travel = position > self->potArmPosition
                ? position - self->potArmPosition
                : self->potArmPosition - position;
            if (travel >= 0.01f)
                self->potMoved = true;
            const bool crossed = self->potMoved &&
                ((self->potPrevious < target && position >= target) ||
                 (self->potPrevious > target && position <= target));
            const float distance = position > target ? position - target : target - position;
            self->potCaught = crossed || (self->potMoved && distance <= 0.005f);
        }
        if (self->potCaught)
            setSelectionGain(self,
                kMute + static_cast<int>(position * (kMaxGain - kMute) + 0.5f),
                true);
        self->potPrevious = position;
    }
}

void setupUi(_NT_algorithm* algorithm, _NT_float3& pots) {
    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    normalizeSelection(self);
    pots[1] = (selectedValue(self) - kMute) /
              static_cast<float>(kMaxGain - kMute);
    self->potCaught = false;
    self->potHasPrevious = false;
    self->potMoved = false;
}

void serialise(_NT_algorithm* algorithm, _NT_jsonStream& stream) {
    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    stream.addMemberName("matrix");
    stream.openArray();
    for (int crosspoint = 0; crosspoint < kCrosspoints; ++crosspoint)
        stream.addNumber(static_cast<int>(self->gainValues[crosspoint]));
    stream.closeArray();

    stream.addMemberName("midiMap");
    stream.openArray();
    for (int target = 0; target < kTargets; ++target)
        stream.addNumber(static_cast<int>(self->midiMappings[target]));
    stream.closeArray();

    stream.addMemberName("muteRestore");
    stream.openArray();
    for (int target = 0; target < kTargets; ++target)
        stream.addNumber(static_cast<int>(self->muteRestoreValues[target]));
    stream.closeArray();
}

bool deserialise(_NT_algorithm* algorithm, _NT_jsonParse& parse) {
    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    int members = 0;
    if (!parse.numberOfObjectMembers(members))
        return false;
    for (int member = 0; member < members; ++member) {
        if (parse.matchName("matrix")) {
            int count = 0;
            if (!parse.numberOfArrayElements(count) || count != kCrosspoints)
                return false;
            for (int crosspoint = 0; crosspoint < kCrosspoints; ++crosspoint) {
                int value = 0;
                if (!parse.number(value))
                    return false;
                setCrosspointValue(self, crosspoint, value);
            }
        } else if (parse.matchName("midiMap")) {
            int count = 0;
            if (!parse.numberOfArrayElements(count) || count != kTargets)
                return false;
            self->hasMidiMappings = false;
            for (int target = 0; target < kTargets; ++target) {
                int value = kNoMidiMapping;
                if (!parse.number(value))
                    return false;
                if (value < kNoMidiMapping || value > 2047)
                    value = kNoMidiMapping;
                self->midiMappings[target] = static_cast<int16_t>(value);
                if (value != kNoMidiMapping)
                    self->hasMidiMappings = true;
            }
        } else if (parse.matchName("muteRestore")) {
            int count = 0;
            if (!parse.numberOfArrayElements(count) || count != kTargets)
                return false;
            for (int target = 0; target < kTargets; ++target) {
                int value = kNotMuted;
                if (!parse.number(value))
                    return false;
                if (value != kNotMuted && (value < kMute || value > kMaxGain))
                    value = kNotMuted;
                self->muteRestoreValues[target] = static_cast<int16_t>(value);
            }
        } else {
            if (!parse.skipMember())
                return false;
        }
    }
    return true;
}

int parameterUiPrefix(_NT_algorithm*, int p, char* buffer) {
    if (p == kParamFirstDirectOutput || p == kParamFirstAuxOutput ||
        p >= kParamRouteInput)
        return 0;
    if (p >= kParamFirstInputBus && p < kParamFirstInputBus + kInputs)
        return std::snprintf(buffer, kNT_parameterUiPrefixSize, "I%u ",
                             p - kParamFirstInputBus + 1);
    if (p >= kParamFirstOutputBus && p < kParamFirstInputTrim)
        return std::snprintf(buffer, kNT_parameterUiPrefixSize, "O%u ",
                             (p - kParamFirstOutputBus) / 2 + 1);
    if (p >= kParamFirstOutputTrim)
        return std::snprintf(buffer, kNT_parameterUiPrefixSize, "O%u ",
                             p - kParamFirstOutputTrim + 1);
    if (p >= kParamFirstInputTrim)
        return std::snprintf(buffer, kNT_parameterUiPrefixSize, "I%u ",
                             p - kParamFirstInputTrim + 1);
    return 0;
}

const _NT_factory factory = {
    NT_MULTICHAR('M', 'x', '1', '6'), "Matrix 12x16",
    "12 input, 16 output matrix mixer", 0, nullptr,
    nullptr, nullptr, calculateRequirements, construct, parameterChanged, step,
    draw, nullptr, midiMessage, kNT_tagUtility, hasCustomUi, customUi, setupUi,
    serialise, deserialise, nullptr, parameterUiPrefix, nullptr
};

} // namespace

extern "C" uintptr_t pluginEntry(_NT_selector selector, uint32_t) {
    switch (selector) {
    case kNT_selector_version: return kNT_apiVersionCurrent;
    case kNT_selector_numFactories: return 1;
    case kNT_selector_factoryInfo: return reinterpret_cast<uintptr_t>(&factory);
    default: return 0;
    }
}
