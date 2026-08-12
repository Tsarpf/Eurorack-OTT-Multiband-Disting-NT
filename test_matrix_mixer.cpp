#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "matrix_mixer.cpp"

namespace {
_NT_algorithm* testAlgorithm = nullptr;
uint32_t testCycles = 0;

void setLocal(int parameter, int16_t value) {
    int16_t* values = const_cast<int16_t*>(testAlgorithm->v);
    values[parameter] = value;
    parameterChanged(testAlgorithm, parameter);
}

bool closeEnough(float actual, float expected, float tolerance = 1.0e-5f) {
    return std::fabs(actual - expected) <= tolerance;
}
} // namespace

extern "C" {
uint8_t NT_screen[128 * 64] = {};
const _NT_globals NT_globals = {48000, 64, nullptr, 0, 0, 0};

uint32_t NT_getCpuCycleCount() {
    testCycles += 60000000u;
    return testCycles;
}

int32_t NT_algorithmIndex(const _NT_algorithm*) {
    return 0;
}

uint32_t NT_parameterOffset() {
    return 1;
}

void NT_setParameterFromAudio(uint32_t, uint32_t parameter, int16_t value) {
    setLocal(static_cast<int>(parameter - NT_parameterOffset()), value);
}

void NT_setParameterFromUi(uint32_t, uint32_t parameter, int16_t value) {
    setLocal(static_cast<int>(parameter - NT_parameterOffset()), value);
}

void NT_drawText(int, int, const char*, int, _NT_textAlignment, _NT_textSize) {}
void NT_drawShapeI(_NT_shape, int, int, int, int, int) {}
}

void _NT_jsonStream::addMemberName(const char*) {}
void _NT_jsonStream::openArray() {}
void _NT_jsonStream::closeArray() {}
void _NT_jsonStream::addNumber(int) {}
bool _NT_jsonParse::numberOfObjectMembers(int&) { return false; }
bool _NT_jsonParse::numberOfArrayElements(int&) { return false; }
bool _NT_jsonParse::matchName(const char*) { return false; }
bool _NT_jsonParse::skipMember() { return false; }
bool _NT_jsonParse::number(int&) { return false; }

int main() {
    _NT_algorithmRequirements requirements = {};
    calculateRequirements(requirements, nullptr);
    assert(requirements.numParameters == kNumParameters);
    assert(kNumParameters <= 255);
    assert(parameters[inputBusParam(0)].unit == kNT_unitAudioInput);
    assert(parameters[inputBusParam(11)].unit == kNT_unitAudioInput);
    assert(parameters[destinationParam(0)].unit == kNT_unitAudioOutput);
    assert(parameters[destinationParam(15)].unit == kNT_unitAudioOutput);
    assert(parameters[outputModeParam(0)].unit == kNT_unitOutputMode);
    assert(parameters[outputModeParam(15)].unit == kNT_unitOutputMode);

    alignas(MatrixMixer) uint8_t storage[sizeof(MatrixMixer)] = {};
    _NT_algorithmMemoryPtrs pointers = {storage, nullptr, nullptr, nullptr};
    _NT_algorithm* algorithm = construct(pointers, requirements, nullptr);
    std::vector<int16_t> values(kNumParameters);
    for (int parameter = 0; parameter < kNumParameters; ++parameter)
        values[parameter] = parameters[parameter].def;
    algorithm->v = values.data();
    algorithm->vIncludingCommon = values.data();
    testAlgorithm = algorithm;
    for (int parameter = 0; parameter < kNumParameters; ++parameter)
        parameterChanged(algorithm, parameter);

    MatrixMixer* self = static_cast<MatrixMixer*>(algorithm);
    assert(levelBlockCount(kMute) == 0);
    assert(levelBlockCount(-540) == 1);
    assert(levelBlockCount(0) == 10);
    assert(levelBlockCount(kMaxGain) == 11);
    setLocal(kParamRouteInput, 12);
    setLocal(kParamRouteOutput, 4);
    setLocal(kParamRouteLevel, -30);
    assert(self->gainValues[crosspointIndex(11, 3)] == kMute);
    setLocal(kParamApplyRoute, 1);
    assert(self->gainValues[crosspointIndex(11, 3)] == -30);
    assert(values[kParamApplyRoute] == 0);
    assert(values[kParamStateRevision] == 1);

    constexpr int frames = 16;
    std::vector<float> buses(kNT_lastBus * frames, 0.0f);
    float* input12 = buses.data() + 11 * frames;
    for (int frame = 0; frame < frames; ++frame)
        input12[frame] = -0.75f + frame * 0.1f;

    // Add mode must preserve an earlier writer and add the selected route.
    for (int output = 0; output < 8; ++output) {
        float* destination = buses.data() + (12 + output) * frames;
        for (int frame = 0; frame < frames; ++frame)
            destination[frame] = 0.25f;
    }
    setCrosspointValue(self, crosspointIndex(11, 0), 0);
    step(algorithm, buses.data(), frames / 4);
    float* direct1 = buses.data() + 12 * frames;
    float* direct2 = buses.data() + 13 * frames;
    for (int frame = 0; frame < frames; ++frame) {
        assert(closeEnough(direct1[frame], input12[frame] + 0.25f));
        assert(closeEnough(direct2[frame], 0.25f));
    }
    // NT busses carry volts, so a 0.75 V peak occupies 7.5% of the ±10 V meter.
    assert(closeEnough(self->inputMeters[11], 0.075f));
    assert(self->outputMeters[0] > 0.0f);

    // Replace mode is independent per output: the selected destination copies
    // the mix while a neighbouring Add destination remains untouched.
    setLocal(outputModeParam(0), 1);
    for (int output = 0; output < 8; ++output) {
        float* destination = buses.data() + (12 + output) * frames;
        for (int frame = 0; frame < frames; ++frame)
            destination[frame] = 0.25f;
    }
    step(algorithm, buses.data(), frames / 4);
    for (int frame = 0; frame < frames; ++frame) {
        assert(closeEnough(direct1[frame], input12[frame]));
        assert(closeEnough(direct2[frame], 0.25f));
    }

    // The second half targets aux 1-8 independently.
    setCrosspointValue(self, crosspointIndex(11, 8), 0);
    setLocal(outputModeParam(8), 1);
    step(algorithm, buses.data(), frames / 4);
    float* aux1 = buses.data() + 20 * frames;
    for (int frame = 0; frame < frames; ++frame)
        assert(closeEnough(aux1[frame], input12[frame]));

    // A disabled expander group must not touch its buses even when the saved
    // destination mode is Replace and a crosspoint remains active.
    setLocal(kParamAuxOutputCount, 0);
    for (int frame = 0; frame < frames; ++frame)
        aux1[frame] = 0.375f;
    step(algorithm, buses.data(), frames / 4);
    for (int frame = 0; frame < frames; ++frame)
        assert(closeEnough(aux1[frame], 0.375f));

    // Internal outputs use the same release semantics and leave the direct
    // destination untouched when their configured count is zero.
    setLocal(kParamDirectOutputCount, 0);
    for (int frame = 0; frame < frames; ++frame)
        direct1[frame] = 0.625f;
    step(algorithm, buses.data(), frames / 4);
    for (int frame = 0; frame < frames; ++frame)
        assert(closeEnough(direct1[frame], 0.625f));
    setLocal(kParamDirectOutputCount, 8);

    // Reducing the input count similarly removes higher sources from DSP.
    setLocal(kParamInputCount, 11);
    setLocal(outputModeParam(0), 0);
    for (int frame = 0; frame < frames; ++frame)
        direct1[frame] = 0.125f;
    step(algorithm, buses.data(), frames / 4);
    for (int frame = 0; frame < frames; ++frame)
        assert(closeEnough(direct1[frame], 0.125f));
    setLocal(kParamInputCount, 12);
    setLocal(kParamAuxOutputCount, 8);

    // Left/right encoders remain strictly row/column selectors.
    self->editing = false;
    self->row = 4;
    self->column = 6;
    _NT_uiData ui = {};
    ui.encoders[1] = 1;
    customUi(algorithm, ui);
    assert(self->row == 4 && self->column == 7);
    ui = {};
    ui.encoders[0] = 1;
    customUi(algorithm, ui);
    assert(self->row == 5 && self->column == 7);

    // With two internal and one expander output, navigation skips directly
    // across the disabled columns at the 8+8 group boundary.
    setLocal(kParamDirectOutputCount, 2);
    setLocal(kParamAuxOutputCount, 1);
    self->row = 0;
    self->column = 2;
    ui = {};
    ui.encoders[1] = 1;
    customUi(algorithm, ui);
    assert(self->column == 9);
    ui = {};
    ui.encoders[1] = 1;
    customUi(algorithm, ui);
    assert(self->column == 0);
    setLocal(kParamDirectOutputCount, 8);
    setLocal(kParamAuxOutputCount, 8);

    // There is no invalid bottom-left cell: moving upward from input 1 trim
    // wraps to input 12 trim, not back to input 1.
    self->row = 0;
    self->column = 0;
    ui = {};
    ui.encoders[0] = -1;
    customUi(algorithm, ui);
    assert(self->row == 11 && self->column == 0);

    // A slow encoder detent uses 0.1 dB and crossing unity lands on 0.0 dB.
    self->row = 11;
    self->column = 1;
    setCrosspointValue(self, crosspointIndex(11, 0), -1);
    ui = {};
    ui.controls = kNT_encoderButtonR;
    customUi(algorithm, ui);
    assert(!self->editing);
    ui = {};
    ui.lastButtons = kNT_encoderButtonR;
    customUi(algorithm, ui);
    assert(self->editing);
    ui = {};
    ui.encoders[1] = 1;
    customUi(algorithm, ui);
    assert(self->gainValues[crosspointIndex(11, 0)] == 0);

    // Left-pot click toggles mute and restores the exact pre-mute level.
    self->editing = false;
    self->selectionAxis = kSelectionSingle;
    self->row = 2;
    self->column = 3;
    const int muteTarget = crosspointIndex(2, 2);
    setCrosspointValue(self, muteTarget, -123);
    ui = {};
    ui.controls = kNT_potButtonL;
    customUi(algorithm, ui);
    assert(self->gainValues[muteTarget] == kMute);
    assert(self->muteRestoreValues[muteTarget] == -123);
    ui = {};
    ui.controls = kNT_potButtonL;
    customUi(algorithm, ui);
    assert(self->gainValues[muteTarget] == -123);
    assert(self->muteRestoreValues[muteTarget] == kNotMuted);

    // Right-pot click learns the next MIDI CC, including its channel, and the
    // learned CC controls the internal crosspoint directly.
    ui = {};
    ui.controls = kNT_potButtonR;
    customUi(algorithm, ui);
    assert(self->midiLearnArmed);
    assert(midiLearnIncludes(self, muteTarget));
    assert(!midiLearnIncludes(self, crosspointIndex(2, 3)));
    midiMessage(algorithm, 0xb2, 74, 127);
    assert(!self->midiLearnArmed);
    assert(self->midiMappings[muteTarget] == ((2 << 7) | 74));
    assert(self->gainValues[muteTarget] == kMaxGain);
    midiMessage(algorithm, 0xb2, 75, 0);
    assert(self->gainValues[muteTarget] == kMaxGain);
    midiMessage(algorithm, 0xb2, 74, 0);
    assert(self->gainValues[muteTarget] == kMute);

    // Right encoder press-turn selects a contiguous run in one row. The
    // release keeps the run selected, and a subsequent turn edits every cell.
    self->editing = false;
    self->selectionAxis = kSelectionSingle;
    self->row = 2;
    self->column = 1;
    setCrosspointValue(self, crosspointIndex(2, 0), -100);
    setCrosspointValue(self, crosspointIndex(2, 1), -200);
    setCrosspointValue(self, crosspointIndex(2, 2), -300);
    ui = {};
    ui.controls = kNT_encoderButtonR;
    customUi(algorithm, ui);
    ui = {};
    ui.controls = kNT_encoderButtonR;
    ui.lastButtons = kNT_encoderButtonR;
    ui.encoders[1] = 2;
    customUi(algorithm, ui);
    assert(self->editing && self->selectionAxis == kSelectionHorizontal);
    assert(self->selectionAnchorColumn == 1 && self->column == 3);
    assert(cellInSelection(self, 2, 1));
    assert(cellInSelection(self, 2, 2));
    assert(cellInSelection(self, 2, 3));
    assert(!cellInSelection(self, 3, 2));
    ui = {};
    ui.lastButtons = kNT_encoderButtonR;
    customUi(algorithm, ui);
    assert(self->editing);
    ui = {};
    ui.encoders[1] = 1;
    customUi(algorithm, ui);
    assert(self->gainValues[crosspointIndex(2, 0)] == -99);
    assert(self->gainValues[crosspointIndex(2, 1)] == -199);
    assert(self->gainValues[crosspointIndex(2, 2)] == -299);

    // MIDI learn snapshots the complete active selection, not merely the
    // current endpoint. One learned CC then controls every selected cell.
    ui = {};
    ui.controls = kNT_potButtonR;
    customUi(algorithm, ui);
    assert(self->midiLearnArmed);
    assert(midiLearnIncludes(self, crosspointIndex(2, 0)));
    assert(midiLearnIncludes(self, crosspointIndex(2, 1)));
    assert(midiLearnIncludes(self, crosspointIndex(2, 2)));
    assert(!midiLearnIncludes(self, crosspointIndex(2, 3)));
    midiMessage(algorithm, 0xb4, 21, 64);
    const int learnedGain = midiValueToGain(64);
    assert(!self->midiLearnArmed);
    for (int output = 0; output < 3; ++output) {
        const int target = crosspointIndex(2, output);
        assert(self->midiMappings[target] == ((4 << 7) | 21));
        assert(self->gainValues[target] == learnedGain);
    }
    assert(self->midiMappings[crosspointIndex(2, 3)] == kNoMidiMapping);

    // Left encoder press-turn makes the corresponding one-column selection.
    self->editing = false;
    self->selectionAxis = kSelectionSingle;
    self->row = 0;
    self->column = 4;
    ui = {};
    ui.controls = kNT_encoderButtonL;
    customUi(algorithm, ui);
    ui = {};
    ui.controls = kNT_encoderButtonL;
    ui.lastButtons = kNT_encoderButtonL;
    ui.encoders[0] = 2;
    customUi(algorithm, ui);
    assert(self->editing && self->selectionAxis == kSelectionVertical);
    assert(self->selectionAnchorRow == 0 && self->row == 2);
    assert(cellInSelection(self, 0, 4));
    assert(cellInSelection(self, 1, 4));
    assert(cellInSelection(self, 2, 4));
    assert(!cellInSelection(self, 1, 5));

    // Pot movement cannot alter the selected level until it reaches/crosses
    // the selected level's virtual position.
    self->editing = true;
    self->selectionAxis = kSelectionSingle;
    self->row = 11;
    self->column = 1;
    _NT_float3 pots = {};
    setupUi(algorithm, pots);
    const int beforePickup = self->gainValues[crosspointIndex(11, 0)];
    ui = {};
    ui.controls = kNT_potC;
    ui.pots[1] = 0.10f;
    customUi(algorithm, ui);
    assert(self->gainValues[crosspointIndex(11, 0)] == beforePickup);
    ui.pots[1] = 0.50f;
    customUi(algorithm, ui);
    assert(self->gainValues[crosspointIndex(11, 0)] == beforePickup);
    ui.pots[1] = 0.95f;
    customUi(algorithm, ui);
    assert(self->potCaught);
    assert(self->gainValues[crosspointIndex(11, 0)] > beforePickup);

    std::puts("matrix mixer tests passed");
    return 0;
}
