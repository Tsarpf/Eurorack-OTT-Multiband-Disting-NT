#include "../distingnt_api/include/distingnt/api.h"
#include "../distingnt_api/include/distingnt/serialisation.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

const _NT_globals NT_globals = {
    .sampleRate = 48000,
    .maxFramesPerStep = 24,
    .workBuffer = nullptr,
    .workBufferSizeBytes = 0,
};
uint8_t NT_screen[128 * 64];

void NT_drawText(int, int, const char *, int, _NT_textAlignment, _NT_textSize) {}
void NT_drawShapeI(_NT_shape, int, int, int, int, int) {}
void NT_setParameterFromUi(int, int, int) {}
int NT_algorithmIndex(_NT_algorithm *) { return 0; }
uint32_t NT_parameterOffset(void) { return 0; }
uint32_t NT_getCpuCycleCount(void) { return 0; }

void _NT_jsonStream::addMemberName(const char *) {}
void _NT_jsonStream::addNumber(int) {}
void _NT_jsonStream::addNumber(float) {}
bool _NT_jsonParse::numberOfObjectMembers(int &num) {
  num = 0;
  return true;
}
bool _NT_jsonParse::matchName(const char *) { return false; }
bool _NT_jsonParse::number(int &) { return false; }
bool _NT_jsonParse::skipMember(void) { return true; }

bool draw(_NT_algorithm *) { return false; }
uint32_t hasCustomUi(_NT_algorithm *) { return 0; }
void customUi(_NT_algorithm *, const _NT_uiData &) {}
void setupUi(_NT_algorithm *, _NT_float3 &) {}

#include "vocoder_algo.cpp"

static void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "TEST FAILED: " << message << "\n";
    std::exit(1);
  }
}

static bool nearlyEqual(float a, float b, float tolerance) {
  return fabsf(a - b) <= tolerance;
}

struct HostAlgorithm {
  _NT_algorithm *algorithm;
  std::vector<uint8_t> sram;
  std::vector<uint8_t> dtc;
  int16_t commonValues[16];
  int16_t values[64];
};

static HostAlgorithm makeAlgorithm() {
  HostAlgorithm host = {};

  _NT_algorithmRequirements req;
  factory.calculateRequirements(req, nullptr);
  host.sram.assign(req.sram, 0);
  host.dtc.assign(req.dtc, 0);
  memset(host.commonValues, 0, sizeof(host.commonValues));
  memset(host.values, 0, sizeof(host.values));

  host.values[kInCarrier] = 1;
  host.values[kCarrierStereo] = 0;
  host.values[kInModulator] = 3;
  host.values[kModulatorStereo] = 0;
  host.values[kOut] = 13;
  host.values[kOutMode] = 1;
  host.values[kBandCount] = 8;
  host.values[kBandWidth] = 50;
  host.values[kDepth] = 50;
  host.values[kFormant] = 0;
  host.values[kMinFreq] = 30;
  host.values[kMaxFreq] = 18000;
  host.values[kAttack] = 10;
  host.values[kRelease] = 100;
  host.values[kEnhance] = 0;
  host.values[kWet] = 100;
  host.values[kPreGain] = 0;

  _NT_algorithmMemoryPtrs ptrs = {host.sram.data(), nullptr, host.dtc.data(),
                                  nullptr};
  host.algorithm = factory.construct(ptrs, req, nullptr);
  host.algorithm->vIncludingCommon = host.commonValues;
  host.algorithm->v = host.values;
  for (int p = 0; p < kNumParams; ++p) {
    factory.parameterChanged(host.algorithm, p);
  }
  return host;
}

static float processAndMeasureAbs(HostAlgorithm &host,
                                  const std::vector<float> &carL,
                                  const std::vector<float> &carR,
                                  const std::vector<float> &modL,
                                  const std::vector<float> &modR,
                                  std::vector<float> *outLeft = nullptr,
                                  std::vector<float> *outRight = nullptr) {
  const int frames = (int)carL.size();
  require((int)carR.size() == frames && (int)modL.size() == frames &&
              (int)modR.size() == frames,
          "buffer size mismatch");

  std::vector<float> outL;
  std::vector<float> outR;
  if (outLeft) {
    outLeft->clear();
  }
  if (outRight) {
    outRight->clear();
  }

  float sumAbs = 0.0f;
  const int block = 24;
  const int numBuses = kNT_lastBus;
  const int carrierBusL = host.values[kInCarrier] - 1;
  const bool carrierStereo = host.values[kCarrierStereo] > 0 &&
                             host.values[kInCarrier] < kNT_lastBus;
  const int carrierBusR = carrierStereo
                              ? carrierBusL + 1
                              : carrierBusL;
  const int modulatorBusL = host.values[kInModulator] - 1;
  const bool modulatorStereo = host.values[kModulatorStereo] > 0 &&
                               host.values[kInModulator] < kNT_lastBus;
  const int modulatorBusR = modulatorStereo
                                ? modulatorBusL + 1
                                : modulatorBusL;
  const int outBusL = host.values[kOut] - 1;
  const bool stereoOutput = (carrierStereo || modulatorStereo) &&
                            host.values[kOut] < kNT_lastBus;
  const int outBusR = stereoOutput ? outBusL + 1 : outBusL;
  for (int offset = 0; offset < frames; offset += block) {
    float bus[block * numBuses];
    memset(bus, 0, sizeof(bus));
    for (int i = 0; i < block; ++i) {
      bus[carrierBusL * block + i] = carL[offset + i];
      if (carrierBusR != carrierBusL) {
        bus[carrierBusR * block + i] = carR[offset + i];
      }
      bus[modulatorBusL * block + i] = modL[offset + i];
      if (modulatorBusR != modulatorBusL) {
        bus[modulatorBusR * block + i] = modR[offset + i];
      }
    }

    factory.step(host.algorithm, bus, block / 4);
    for (int i = 0; i < block; ++i) {
      const float sampleL = bus[outBusL * block + i];
      sumAbs += fabsf(sampleL);
      outL.push_back(sampleL);
      if (outRight) {
        outR.push_back(bus[outBusR * block + i]);
      }
    }
  }

  if (outLeft) {
    *outLeft = outL;
  }
  if (outRight) {
    *outRight = outR;
  }
  return sumAbs / (float)frames;
}

static void testDescriptorLayout() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  rebuildDescriptor(algo);

  require(algo->descriptor->activeBands == 8, "descriptor active band count");
  require(nearlyEqual(algo->descriptor->analysisFreq[0], 30.0f, 0.01f),
          "first band frequency");
  require(nearlyEqual(algo->descriptor->analysisFreq[7], 18000.0f, 1.0f),
          "last band frequency");
  require(algo->descriptor->synthesisQ >= 3.0f, "synthesis Q floor");
  require(algo->descriptor->bandwidthCompensation > 0.0f,
          "bandwidth compensation positive");
}

static void testWetZeroPassthrough() {
  HostAlgorithm host = makeAlgorithm();
  host.values[kWet] = 0;
  factory.parameterChanged(host.algorithm, kWet);
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  algo->controls.currentWet = 0.0f;
  algo->controls.targetWet = 0.0f;

  const int frames = 96;
  std::vector<float> carL(frames), carR(frames), modL(frames), modR(frames);
  for (int i = 0; i < frames; ++i) {
    carL[i] = 0.4f * sinf(2.0f * 3.14159265359f * 200.0f * i / 48000.0f);
    carR[i] = 0.3f * cosf(2.0f * 3.14159265359f * 170.0f * i / 48000.0f);
    modL[i] = (i & 1) ? 0.5f : -0.5f;
    modR[i] = modL[i];
  }

  std::vector<float> out;
  processAndMeasureAbs(host, carL, carR, modL, modR, &out);
  for (int i = 0; i < frames; ++i) {
    require(nearlyEqual(out[i], carL[i], 1.0e-5f), "wet=0 passthrough");
  }
}

static void testMonoDefaultIgnoresNextCarrierBus() {
  HostAlgorithm hostA = makeAlgorithm();
  HostAlgorithm hostB = makeAlgorithm();

  const int frames = 96;
  std::vector<float> carL(frames), carR(frames), modL(frames), modR(frames);
  for (int i = 0; i < frames; ++i) {
    carL[i] = 0.35f * sinf(2.0f * 3.14159265359f * 220.0f * i / 48000.0f);
    carR[i] = 0.9f * sinf(2.0f * 3.14159265359f * 37.0f * i / 48000.0f);
    modL[i] = 0.4f * sinf(2.0f * 3.14159265359f * 400.0f * i / 48000.0f);
    modR[i] = modL[i];
  }

  std::vector<float> outA, outB;
  processAndMeasureAbs(hostA, carL, carR, modL, modR, &outA);
  std::fill(carR.begin(), carR.end(), 0.0f);
  processAndMeasureAbs(hostB, carL, carR, modL, modR, &outB);

  for (int i = 0; i < frames; ++i) {
    require(nearlyEqual(outA[i], outB[i], 1.0e-5f),
            "mono default should ignore next carrier bus");
  }
}

static void testStereoCarrierChangesOutput() {
  HostAlgorithm mono = makeAlgorithm();
  HostAlgorithm stereo = makeAlgorithm();
  stereo.values[kCarrierStereo] = 1;
  factory.parameterChanged(stereo.algorithm, kCarrierStereo);

  const int frames = 240;
  std::vector<float> carL(frames), carR(frames), modL(frames), modR(frames);
  for (int i = 0; i < frames; ++i) {
    carL[i] = 0.35f * sinf(2.0f * 3.14159265359f * 110.0f * i / 48000.0f);
    carR[i] = 0.35f * sinf(2.0f * 3.14159265359f * 220.0f * i / 48000.0f);
    modL[i] = 0.5f * sinf(2.0f * 3.14159265359f * 340.0f * i / 48000.0f);
    modR[i] = modL[i];
  }

  std::vector<float> outMonoL, outStereoL, outStereoR;
  processAndMeasureAbs(mono, carL, carR, modL, modR, &outMonoL);
  processAndMeasureAbs(stereo, carL, carR, modL, modR, &outStereoL, &outStereoR);

  float diff = 0.0f;
  for (int i = 0; i < frames; ++i) {
    diff += fabsf(outStereoL[i] - outStereoR[i]);
  }
  require(diff > 1.0e-4f, "carrier stereo toggle should produce distinct right output");

  float leftDiff = 0.0f;
  for (int i = 0; i < frames; ++i) {
    leftDiff += fabsf(outMonoL[i] - outStereoL[i]);
  }
  require(leftDiff < 1.0e-4f,
          "carrier stereo toggle should not change left channel output");
}

static void testHighBusStereoInputsUseNextBus() {
  const int frames = 2400;
  std::vector<float> carL(frames), carR(frames), modL(frames), modR(frames);
  for (int i = 0; i < frames; ++i) {
    const float phase = 2.0f * 3.14159265359f * (float)i / 48000.0f;
    carL[i] = 0.25f * sinf(220.0f * phase);
    carR[i] = 0.35f * sinf(330.0f * phase);
    modL[i] = 0.45f * sinf(400.0f * phase);
    modR[i] = 0.45f * sinf(1600.0f * phase);
  }

  // Carrier bus 63 must pair with bus 64. Wet=0 makes this an exact routing
  // check, independent of the filter/envelope state.
  HostAlgorithm carrier = makeAlgorithm();
  carrier.values[kInCarrier] = kNT_lastBus - 1;
  carrier.values[kCarrierStereo] = 1;
  carrier.values[kWet] = 0;
  factory.parameterChanged(carrier.algorithm, kInCarrier);
  factory.parameterChanged(carrier.algorithm, kCarrierStereo);
  factory.parameterChanged(carrier.algorithm, kWet);
  auto *carrierAlgorithm = (_vocoderAlgorithm *)carrier.algorithm;
  carrierAlgorithm->controls.currentWet = 0.0f;
  carrierAlgorithm->controls.targetWet = 0.0f;
  std::vector<float> carrierOutL, carrierOutR;
  processAndMeasureAbs(carrier, carL, carR, modL, modR, &carrierOutL,
                       &carrierOutR);
  for (int i = 0; i < frames; ++i) {
    require(nearlyEqual(carrierOutL[i], carL[i], 1.0e-5f) &&
                nearlyEqual(carrierOutR[i], carR[i], 1.0e-5f),
            "high carrier bus did not use its API 13 stereo partner");
  }

  // The same boundary applies to the modulator input. With a mono carrier,
  // distinct bus-63/64 modulators must produce distinct stereo envelopes.
  HostAlgorithm modulator = makeAlgorithm();
  modulator.values[kCarrierStereo] = 0;
  modulator.values[kInModulator] = kNT_lastBus - 1;
  modulator.values[kModulatorStereo] = 1;
  factory.parameterChanged(modulator.algorithm, kCarrierStereo);
  factory.parameterChanged(modulator.algorithm, kInModulator);
  factory.parameterChanged(modulator.algorithm, kModulatorStereo);
  std::vector<float> modulatorOutL, modulatorOutR;
  processAndMeasureAbs(modulator, carL, carR, modL, modR, &modulatorOutL,
                       &modulatorOutR);
  float modulatorDifference = 0.0f;
  for (int i = frames / 2; i < frames; ++i) {
    modulatorDifference += fabsf(modulatorOutL[i] - modulatorOutR[i]);
  }
  require(modulatorDifference > 1.0e-3f,
          "high modulator bus did not use its API 13 stereo partner");
}

static void testAliasedStereoInputsCollapseAndResynchronise() {
  HostAlgorithm host = makeAlgorithm();
  host.values[kCarrierStereo] = 1;
  host.values[kModulatorStereo] = 1;
  factory.parameterChanged(host.algorithm, kCarrierStereo);
  factory.parameterChanged(host.algorithm, kModulatorStereo);

  const int block = 24;
  const int outBusL = host.values[kOut] - 1;
  auto *algorithm = (_vocoderAlgorithm *)host.algorithm;
  VocoderDSPState &state = *algorithm->state;

  // Establish an actually-stereo callback, then make the right channel's
  // representative analysis/synthesis/envelope state visibly divergent.
  std::vector<float> stereoBus(kNT_lastBus * block, 0.0f);
  for (int i = 0; i < block; ++i) {
    stereoBus[i] = 0.2f * sinf(0.07f * i);
    stereoBus[block + i] = 0.3f * sinf(0.13f * i);
    stereoBus[2 * block + i] = 0.4f * sinf(0.19f * i);
    stereoBus[3 * block + i] = 0.5f * sinf(0.29f * i);
  }
  factory.step(host.algorithm, stereoBus.data(), block / 4);
  require(state.stereoOutputWasActive,
          "valid stereo inputs were not recognised as stereo");
  state.anState[1][0].state[0] = state.anState[0][0].state[0] + 0.75f;
  state.syState[1][0].state[1] = state.syState[0][0].state[1] - 0.5f;
  state.env[1][0] = state.env[0][0] + 0.25f;
  state.eAvg[1][0] = state.eAvg[0][0] + 0.125f;
  state.gainState[1][0] = state.gainState[0][0] + 0.5f;

  // At bus 64 neither stereo toggle has an addressable +1 partner. The
  // algorithm must expose one output only; bus Output+1 remains untouched.
  host.values[kInCarrier] = kNT_lastBus;
  host.values[kInModulator] = kNT_lastBus;
  factory.parameterChanged(host.algorithm, kInCarrier);
  factory.parameterChanged(host.algorithm, kInModulator);
  std::vector<float> aliasedBus(kNT_lastBus * block, 0.0f);
  const float untouched = 0.625f;
  for (int i = 0; i < block; ++i) {
    aliasedBus[(kNT_lastBus - 1) * block + i] = 0.3f * sinf(0.11f * i);
    aliasedBus[(outBusL + 1) * block + i] = untouched;
  }
  factory.step(host.algorithm, aliasedBus.data(), block / 4);
  require(!state.stereoOutputWasActive,
          "aliased last-bus inputs were treated as distinct stereo");
  for (int i = 0; i < block; ++i) {
    require(nearlyEqual(aliasedBus[(outBusL + 1) * block + i], untouched,
                        1.0e-7f),
            "effective-mono routing wrote a phantom right output");
  }

  // Reintroducing valid stereo inputs must seed the dormant right DSP state
  // from the current left state. Identical inputs then remain identical on the
  // very first stereo block rather than exposing the deliberately stale state.
  host.values[kInCarrier] = 1;
  host.values[kInModulator] = 3;
  factory.parameterChanged(host.algorithm, kInCarrier);
  factory.parameterChanged(host.algorithm, kInModulator);
  std::fill(stereoBus.begin(), stereoBus.end(), 0.0f);
  for (int i = 0; i < block; ++i) {
    const float carrier = 0.25f * sinf(0.09f * i);
    const float modulator = 0.35f * sinf(0.17f * i);
    stereoBus[i] = stereoBus[block + i] = carrier;
    stereoBus[2 * block + i] = stereoBus[3 * block + i] = modulator;
  }
  factory.step(host.algorithm, stereoBus.data(), block / 4);
  require(state.stereoOutputWasActive,
          "restored stereo inputs were not recognised as stereo");
  for (int i = 0; i < block; ++i) {
    require(nearlyEqual(stereoBus[outBusL * block + i],
                        stereoBus[(outBusL + 1) * block + i], 1.0e-6f),
            "stale right DSP state leaked into restored stereo output");
  }
  for (int band = 0; band < algorithm->activeBands; ++band) {
    require(nearlyEqual(state.anState[0][band].state[0],
                        state.anState[1][band].state[0], 1.0e-7f) &&
                nearlyEqual(state.anState[0][band].state[1],
                            state.anState[1][band].state[1], 1.0e-7f) &&
                nearlyEqual(state.syState[0][band].state[0],
                            state.syState[1][band].state[0], 1.0e-7f) &&
                nearlyEqual(state.syState[0][band].state[1],
                            state.syState[1][band].state[1], 1.0e-7f) &&
                nearlyEqual(state.env[0][band], state.env[1][band],
                            1.0e-7f) &&
                nearlyEqual(state.eAvg[0][band], state.eAvg[1][band],
                            1.0e-7f) &&
                nearlyEqual(state.gainState[0][band],
                            state.gainState[1][band], 1.0e-7f),
            "right analysis/synthesis/envelope state was not resynchronised");
  }
}

static void testSourceSpecificStereoStateTransitions() {
  HostAlgorithm host = makeAlgorithm();
  host.values[kCarrierStereo] = 1;
  host.values[kModulatorStereo] = 1;
  host.commonValues[0] = 1; // isolate routing-state transitions from DSP motion
  factory.parameterChanged(host.algorithm, kCarrierStereo);
  factory.parameterChanged(host.algorithm, kModulatorStereo);

  const int block = 24;
  std::vector<float> bus(kNT_lastBus * block, 0.0f);
  factory.step(host.algorithm, bus.data(), block / 4);
  auto *algorithm = (_vocoderAlgorithm *)host.algorithm;
  VocoderDSPState &state = *algorithm->state;
  require(state.carrierStereoWasActive && state.modulatorStereoWasActive &&
              state.stereoOutputWasActive,
          "initial source-specific stereo state was not recorded");

  // The carrier stays valid stereo while the modulator loses its +1 bus.
  // Only analysis/envelope/gain and modulator DC state should be unified.
  state.anState[0][0].state[0] = 0.11f;
  state.anState[1][0].state[0] = 0.91f;
  state.env[0][0] = 0.12f;
  state.env[1][0] = 0.92f;
  state.eAvg[0][0] = 0.13f;
  state.eAvg[1][0] = 0.93f;
  state.gainState[0][0] = 0.14f;
  state.gainState[1][0] = 0.94f;
  state.modDcX1[0] = 0.15f;
  state.modDcX1[1] = 0.95f;
  state.syState[0][0].state[0] = 0.21f;
  state.syState[1][0].state[0] = 0.81f;
  state.carrierDcX1[0] = 0.22f;
  state.carrierDcX1[1] = 0.82f;
  host.values[kInModulator] = kNT_lastBus;
  factory.parameterChanged(host.algorithm, kInModulator);
  factory.step(host.algorithm, bus.data(), block / 4);
  require(state.carrierStereoWasActive && !state.modulatorStereoWasActive &&
              state.stereoOutputWasActive,
          "mono modulator incorrectly collapsed the stereo carrier output");
  require(nearlyEqual(state.anState[1][0].state[0], 0.11f, 1.0e-7f) &&
              nearlyEqual(state.env[1][0], 0.12f, 1.0e-7f) &&
              nearlyEqual(state.eAvg[1][0], 0.13f, 1.0e-7f) &&
              nearlyEqual(state.gainState[1][0], 0.14f, 1.0e-7f) &&
              nearlyEqual(state.modDcX1[1], 0.15f, 1.0e-7f),
          "mono modulator did not synchronise analysis/envelope/gain state");
  require(nearlyEqual(state.syState[1][0].state[0], 0.81f, 1.0e-7f) &&
              nearlyEqual(state.carrierDcX1[1], 0.82f, 1.0e-7f),
          "modulator transition overwrote independent carrier state");

  // Restore the modulator, then make only the carrier effectively mono. Its
  // DC/synthesis state must unify without erasing distinct modulator history.
  host.values[kInModulator] = 3;
  factory.parameterChanged(host.algorithm, kInModulator);
  factory.step(host.algorithm, bus.data(), block / 4);
  state.syState[0][0].state[1] = 0.31f;
  state.syState[1][0].state[1] = 0.71f;
  state.carrierDcY1[0] = 0.32f;
  state.carrierDcY1[1] = 0.72f;
  state.anState[0][0].state[1] = 0.41f;
  state.anState[1][0].state[1] = 0.61f;
  state.env[0][0] = 0.42f;
  state.env[1][0] = 0.62f;
  host.values[kInCarrier] = kNT_lastBus;
  factory.parameterChanged(host.algorithm, kInCarrier);
  factory.step(host.algorithm, bus.data(), block / 4);
  require(!state.carrierStereoWasActive && state.modulatorStereoWasActive &&
              state.stereoOutputWasActive,
          "mono carrier incorrectly collapsed the stereo modulator output");
  require(nearlyEqual(state.syState[1][0].state[1], 0.31f, 1.0e-7f) &&
              nearlyEqual(state.carrierDcY1[1], 0.32f, 1.0e-7f),
          "mono carrier did not synchronise synthesis/DC state");
  require(nearlyEqual(state.anState[1][0].state[1], 0.61f, 1.0e-7f) &&
              nearlyEqual(state.env[1][0], 0.62f, 1.0e-7f),
          "carrier transition overwrote independent modulator state");
}

static void testRightOutputDescription() {
  HostAlgorithm host = makeAlgorithm();
  char text[kNT_parameterStringSize] = {};

  require(parameters[kPreGain].unit == kNT_unitDb &&
              parameters[kPreGain].scaling == kNT_scaling10,
          "Pre gain should display in tenths of a decibel");

  parameterString(host.algorithm, kOutRight, 0, text);
  require(strcmp(text, "Same as left") == 0,
          "mono right output description is wrong");

  host.values[kCarrierStereo] = 1;
  parameterString(host.algorithm, kOutRight, 0, text);
  require(strcmp(text, "Output + 1") == 0,
          "stereo right output description is wrong");

  host.values[kInCarrier] = kNT_lastBus;
  parameterString(host.algorithm, kOutRight, 0, text);
  require(strcmp(text, "Same as left") == 0,
          "aliased carrier stereo should report mono output");

  host.values[kModulatorStereo] = 1;
  parameterString(host.algorithm, kOutRight, 0, text);
  require(strcmp(text, "Output + 1") == 0,
          "valid modulator stereo should retain stereo output");

  host.values[kInModulator] = kNT_lastBus;
  parameterString(host.algorithm, kOutRight, 0, text);
  require(strcmp(text, "Same as left") == 0,
          "two aliased stereo inputs should report mono output");

  host.values[kInCarrier] = 1;
  host.values[kOut] = kNT_lastBus;
  parameterString(host.algorithm, kOutRight, 0, text);
  require(strcmp(text, "Same as left") == 0,
          "last-bus stereo output should report its mono fallback");
}

static void testBypassPreservesOverlappingStereoInputs() {
  HostAlgorithm host = makeAlgorithm();
  host.values[kCarrierStereo] = 1;
  host.values[kInCarrier] = 1;
  host.values[kOut] = 2;
  host.values[kOutMode] = 1;
  host.commonValues[0] = 1;

  const int frames = 24;
  std::vector<float> bus(kNT_lastBus * frames, 0.0f);
  for (int i = 0; i < frames; ++i) {
    bus[i] = 0.1f + 0.001f * i;
    bus[frames + i] = -0.2f - 0.002f * i;
  }
  const std::vector<float> original = bus;
  factory.step(host.algorithm, bus.data(), frames / 4);
  for (int i = 0; i < frames; ++i) {
    require(nearlyEqual(bus[frames + i], original[i], 1.0e-7f) &&
                nearlyEqual(bus[2 * frames + i], original[frames + i],
                            1.0e-7f),
            "overlapping stereo bypass corrupted a carrier channel");
  }
}

static void testLastOutputBusUsesMonoFallback() {
  HostAlgorithm host = makeAlgorithm();
  host.values[kCarrierStereo] = 1;
  host.values[kOut] = kNT_lastBus;
  host.values[kOutMode] = 1;
  host.commonValues[0] = 1;

  const int frames = 24;
  std::vector<float> bus(kNT_lastBus * frames, 0.0f);
  for (int i = 0; i < frames; ++i) {
    bus[i] = 0.1f + 0.001f * i;
    bus[frames + i] = -0.2f - 0.002f * i;
  }
  const std::vector<float> original = bus;
  factory.step(host.algorithm, bus.data(), frames / 4);
  for (int i = 0; i < frames; ++i) {
    require(nearlyEqual(bus[(kNT_lastBus - 1) * frames + i], original[i],
                        1.0e-7f),
            "last-bus stereo request did not use the left mono fallback");
  }
}

static void testExtremeDepthPowerInterpolation() {
  const VocoderDepthShape shape = computeDepthShape(800.0f);
  require(nearlyEqual(shape.peakExponent, 9.4f, 1.0e-6f),
          "Depth 800 exponent is wrong");
  const float x = 1.2f;
  const float expected = vocoderLerp(powf(x, 9.0f), powf(x, 10.0f), 0.4f);
  require(nearlyEqual(computeDepthGain(shape, 1.0f, x), expected, 1.0e-4f),
          "extreme Depth does not interpolate the advertised power");
  require(computeDepthGain(shape, 1.0f, 0.5f) >= 0.0f,
          "extreme Depth produced a negative gain");
}

static void testImpulseProducesResponse() {
  HostAlgorithm host = makeAlgorithm();
  const int frames = 240;
  std::vector<float> carL(frames), carR(frames), modL(frames, 0.0f),
      modR(frames, 0.0f);
  modL[0] = 1.0f;
  modR[0] = 1.0f;
  for (int i = 0; i < frames; ++i) {
    const float phase = fmodf(110.0f * i / 48000.0f, 1.0f);
    carL[i] = 2.0f * phase - 1.0f;
    carR[i] = carL[i];
  }

  const float meanAbs = processAndMeasureAbs(host, carL, carR, modL, modR);
  require(meanAbs > 1.0e-4f, "impulse response should produce non-zero output");
}

static void testLegacyEnhanceIsHiddenAndNoop() {
  // Preserve the old parameter index for preset compatibility without exposing
  // a control that has no DSP implementation.
  for (uint32_t page = 0; page < paramPages.numPages; ++page) {
    for (uint8_t index = 0; index < paramPages.pages[page].numParams; ++index) {
      require(paramPages.pages[page].params[index] != kEnhance,
              "legacy Enhance parameter should not appear on a page");
    }
  }
  HostAlgorithm base = makeAlgorithm();
  HostAlgorithm enhanced = makeAlgorithm();
  enhanced.values[kEnhance] = 1;
  factory.parameterChanged(enhanced.algorithm, kEnhance);

  const int frames = 480;
  std::vector<float> carL(frames), carR(frames), modL(frames), modR(frames);
  for (int i = 0; i < frames; ++i) {
    const float ramp = 0.1f + 0.9f * ((float)i / (float)(frames - 1));
    carL[i] = ramp * sinf(2.0f * 3.14159265359f * 110.0f * i / 48000.0f);
    carR[i] = carL[i];
    modL[i] = 0.7f * sinf(2.0f * 3.14159265359f * 400.0f * i / 48000.0f);
    modR[i] = modL[i];
  }

  std::vector<float> outA, outB;
  processAndMeasureAbs(base, carL, carR, modL, modR, &outA);
  processAndMeasureAbs(enhanced, carL, carR, modL, modR, &outB);

  float diff = 0.0f;
  for (int i = 0; i < frames; ++i) {
    diff += fabsf(outA[i] - outB[i]);
  }
  require(diff < 1.0e-4f, "legacy Enhance parameter should remain a no-op");
}

static void testFormantSmoothingMovesDescriptor() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  rebuildDescriptor(algo);
  const float before = algo->descriptor->synthesisFreq[4];

  host.values[kFormant] = 120;
  factory.parameterChanged(host.algorithm, kFormant);
  updateControlState(algo);
  const float after = algo->descriptor->synthesisFreq[4];

  require(algo->controls.currentFormant > 0.0f &&
              algo->controls.currentFormant < 120.0f,
          "formant smoothing should move gradually");
  require(after > before, "formant shift should raise synthesis frequency");
}

static void testHighFormantBandsFadeBeforeNyquist() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  host.values[kFormant] = 240;
  factory.parameterChanged(host.algorithm, kFormant);

  // Drive the existing control smoother until the +24 st descriptor settles.
  const int block = 24;
  const int numBuses = 28;
  for (int iteration = 0; iteration < 80; ++iteration) {
    float bus[block * numBuses] = {};
    factory.step(host.algorithm, bus, block / 4);
  }

  const VocoderDescriptor &descriptor = *algo->descriptor;
  const float ceiling = 0.49f * 48000.0f;
  bool foundFadedBand = false;
  for (int band = 0; band < descriptor.activeBands; ++band) {
    require(descriptor.synthesisFreq[band] <= ceiling + 0.01f,
            "formant shift put a synthesis filter above the safe ceiling");
    if (descriptor.analysisFreq[band] * 2.0f >= ceiling) {
      foundFadedBand = true;
      require(descriptor.synthesisBandGain[band] < 0.001f,
              "out-of-range high formant band did not fade out");
    }
  }
  require(foundFadedBand, "high-formant fade test did not reach the ceiling");
}

static void testBlockRateCoefficientTimebase() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  VocoderCachedCoeffs &coeffs = algo->blockCoeffs;
  coeffs.lastAttack = host.values[kAttack];
  coeffs.lastRelease = host.values[kRelease];
  coeffs.lastActiveBands = host.values[kBandCount];

  computeBlockCoeffs(algo, 24, 48000.0f);
  const VocoderCachedCoeffs block24 = coeffs;
  const float blockRate24 = 48000.0f / 24.0f;
  require(nearlyEqual(block24.synthesisCoeffMix,
                      vocoderMixCoeffFromSeconds(blockRate24, 0.0015f),
                      1.0e-6f),
          "synthesis coefficient smoothing must use the block rate");
  require(nearlyEqual(block24.synthesisScalarMix,
                      block24.synthesisCoeffMix, 1.0e-7f),
          "synthesis scalar smoothing must share the block timebase");
  require(nearlyEqual(block24.levelAvgRiseMix,
                      vocoderMixCoeffFromSeconds(blockRate24, 0.01f),
                      1.0e-6f),
          "level averaging must use the block rate");
  require(nearlyEqual(block24.makeupRiseMix,
                      vocoderMixCoeffFromSeconds(blockRate24, 0.05f),
                      1.0e-6f),
          "makeup smoothing must use the block rate");
  require(nearlyEqual(block24.guardReleaseMix,
                      vocoderMixCoeffFromSeconds(blockRate24, 0.05f),
                      1.0e-6f),
          "output guard smoothing must use the block rate");

  computeBlockCoeffs(algo, 12, 48000.0f);
  require(nearlyEqual(block24.synthesisCoeffMix,
                      coeffs.synthesisCoeffMix * coeffs.synthesisCoeffMix,
                      2.0e-6f),
          "synthesis smoothing time must be invariant to block size");
  require(nearlyEqual(block24.makeupRiseMix,
                      coeffs.makeupRiseMix * coeffs.makeupRiseMix, 2.0e-6f),
          "makeup smoothing time must be invariant to block size");
  require(nearlyEqual(block24.guardReleaseMix,
                      coeffs.guardReleaseMix * coeffs.guardReleaseMix,
                      2.0e-6f),
          "guard smoothing time must be invariant to block size");
}

static void testLevelControlRampsAcrossBlock() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  VocoderDSPState &state = *algo->state;
  const int numBuses = 28;

  state.wetMakeup[0] = 1.0f;
  state.wetMakeupTarget[0] = 3.0f;
  state.outputGuard[0] = 0.5f;
  state.outputGuardTarget[0] = 0.75f;

  const int firstBlock = 12;
  float firstBus[firstBlock * numBuses] = {};
  factory.step(host.algorithm, firstBus, firstBlock / 4);
  require(nearlyEqual(state.wetMakeup[0], 3.0f, 1.0e-6f),
          "makeup ramp must reach target on a 12-sample block");
  require(nearlyEqual(state.outputGuard[0], 0.75f, 1.0e-6f),
          "guard ramp must reach target on a 12-sample block");

  // Phase 5 produced the next targets. A different-sized callback must use its
  // own N when consuming them, not the preceding block's N.
  const float nextMakeupTarget = state.wetMakeupTarget[0];
  const float nextGuardTarget = state.outputGuardTarget[0];
  const int secondBlock = 24;
  float secondBus[secondBlock * numBuses] = {};
  factory.step(host.algorithm, secondBus, secondBlock / 4);
  require(nearlyEqual(state.wetMakeup[0], nextMakeupTarget, 1.0e-6f),
          "makeup ramp must reach target after a block-size change");
  require(nearlyEqual(state.outputGuard[0], nextGuardTarget, 1.0e-6f),
          "guard ramp must reach target after a block-size change");
}

static void testMonoDoesNotRetainRightLevelGain() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  VocoderDSPState &state = *algo->state;
  const int block = 24;
  const int numBuses = 28;

  state.wetMakeup[1] = 6.0f;
  state.wetMakeupTarget[1] = 6.0f;
  state.outputGuard[1] = 0.25f;
  state.outputGuardTarget[1] = 0.25f;
  float monoBus[block * numBuses] = {};
  factory.step(host.algorithm, monoBus, block / 4);
  require(nearlyEqual(state.wetMakeup[1], state.wetMakeup[0], 1.0e-7f) &&
              nearlyEqual(state.wetMakeupTarget[1],
                          state.wetMakeupTarget[0], 1.0e-7f),
          "mono mode must discard stale right-channel makeup");
  require(nearlyEqual(state.outputGuard[1], state.outputGuard[0], 1.0e-7f) &&
              nearlyEqual(state.outputGuardTarget[1],
                          state.outputGuardTarget[0], 1.0e-7f),
          "mono mode must discard stale right-channel guard gain");

  host.values[kCarrierStereo] = 1;
  float stereoBus[block * numBuses] = {};
  factory.step(host.algorithm, stereoBus, block / 4);
  require(state.wetMakeup[1] < 1.001f,
          "stereo re-enable must not restore stale right-channel makeup");
}

static void testSilentWetDoesNotChargeMakeup() {
  const int block = 24;
  const int numBuses = 28;
  const int frames = 48000;

  HostAlgorithm quiet = makeAlgorithm();
  quiet.values[kDepth] = 100;
  auto *quietAlgo = (_vocoderAlgorithm *)quiet.algorithm;
  float maxMakeupTarget = 1.0f;
  for (int offset = 0; offset < frames; offset += block) {
    float bus[block * numBuses] = {};
    for (int i = 0; i < block; ++i) {
      bus[i] = 0.6f * sinf(2.0f * 3.14159265359f * 110.0f *
                           (float)(offset + i) / 48000.0f);
    }
    factory.step(quiet.algorithm, bus, block / 4);
    if (quietAlgo->state->wetMakeupTarget[0] > maxMakeupTarget) {
      maxMakeupTarget = quietAlgo->state->wetMakeupTarget[0];
    }
  }
  require(maxMakeupTarget < 1.001f,
          "silent wet path must not charge automatic makeup");

  HostAlgorithm recovering = makeAlgorithm();
  recovering.values[kDepth] = 100;
  auto *recoveringAlgo = (_vocoderAlgorithm *)recovering.algorithm;
  recoveringAlgo->state->wetMakeup[0] = 6.0f;
  recoveringAlgo->state->wetMakeupTarget[0] = 6.0f;
  for (int offset = 0; offset < frames; offset += block) {
    float bus[block * numBuses] = {};
    for (int i = 0; i < block; ++i) {
      bus[i] = 0.6f * sinf(2.0f * 3.14159265359f * 110.0f *
                           (float)(offset + i) / 48000.0f);
    }
    factory.step(recovering.algorithm, bus, block / 4);
  }
  require(recoveringAlgo->state->wetMakeupTarget[0] < 1.01f &&
              recoveringAlgo->state->wetMakeup[0] < 1.01f,
          "silent wet path must return stored makeup gently to unity");
}

static void testMotionStaysFinite() {
  HostAlgorithm host = makeAlgorithm();
  const int frames = 24 * 40;
  std::vector<float> carL(frames), carR(frames), modL(frames), modR(frames);
  for (int i = 0; i < frames; ++i) {
    carL[i] = 0.7f * sinf(2.0f * 3.14159265359f * 90.0f * i / 48000.0f);
    carR[i] = 0.7f * sinf(2.0f * 3.14159265359f * 130.0f * i / 48000.0f);
    modL[i] = ((i % 11) - 5) * 0.12f;
    modR[i] = modL[i];
  }

  const int block = 24;
  const int numBuses = 28;
  for (int offset = 0; offset < frames; offset += block) {
    host.values[kFormant] = (int16_t)(120.0f * sinf(offset / 240.0f));
    host.values[kBandWidth] = (int16_t)(50.0f + 40.0f * sinf(offset / 180.0f));
    factory.parameterChanged(host.algorithm, kFormant);
    factory.parameterChanged(host.algorithm, kBandWidth);

    float bus[block * numBuses];
    memset(bus, 0, sizeof(bus));
    for (int i = 0; i < block; ++i) {
      bus[0 * block + i] = carL[offset + i];
      bus[1 * block + i] = carR[offset + i];
      bus[2 * block + i] = modL[offset + i];
      bus[3 * block + i] = modR[offset + i];
    }

    factory.step(host.algorithm, bus, block / 4);
    for (int i = 0; i < block * numBuses; ++i) {
      require(std::isfinite(bus[i]), "motion sweep produced non-finite sample");
    }
  }
}

static void testMetersRespondToModulator() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;

  const int block = 24;
  const int numBuses = 28;

  // Run for 2 seconds of silence — meters should settle near 0
  const int silenceFrames = 48000 * 2;
  for (int offset = 0; offset < silenceFrames; offset += block) {
    float bus[block * numBuses];
    memset(bus, 0, sizeof(bus));
    factory.step(host.algorithm, bus, block / 4);
  }

  float maxMeterSilence = 0.0f;
  for (int b = 0; b < algo->activeBands; ++b) {
    if (algo->state->meters[b] > maxMeterSilence)
      maxMeterSilence = algo->state->meters[b];
  }
  require(maxMeterSilence < 0.05f, "meters should be near zero with no input");

  // Now run with a tone into modulator — meters should rise
  const int signalFrames = 48000;
  for (int offset = 0; offset < signalFrames; offset += block) {
    float bus[block * numBuses];
    memset(bus, 0, sizeof(bus));
    // 400 Hz carrier (bus 1, index 0)
    for (int i = 0; i < block; ++i)
      bus[0 * block + i] = 0.5f * sinf(2.0f * 3.14159265359f * 400.0f * (offset + i) / 48000.0f);
    // 800 Hz modulator (bus 3, index 2)
    for (int i = 0; i < block; ++i)
      bus[2 * block + i] = 0.5f * sinf(2.0f * 3.14159265359f * 800.0f * (offset + i) / 48000.0f);
    factory.step(host.algorithm, bus, block / 4);
  }

  float maxMeterSignal = 0.0f;
  for (int b = 0; b < algo->activeBands; ++b) {
    if (algo->state->meters[b] > maxMeterSignal)
      maxMeterSignal = algo->state->meters[b];
  }
  require(maxMeterSignal > 0.1f, "meters should respond to modulator signal");
}

int main() {
  testDescriptorLayout();
  testWetZeroPassthrough();
  testMonoDefaultIgnoresNextCarrierBus();
  testStereoCarrierChangesOutput();
  testHighBusStereoInputsUseNextBus();
  testAliasedStereoInputsCollapseAndResynchronise();
  testSourceSpecificStereoStateTransitions();
  testRightOutputDescription();
  testBypassPreservesOverlappingStereoInputs();
  testLastOutputBusUsesMonoFallback();
  testExtremeDepthPowerInterpolation();
  testImpulseProducesResponse();
  testLegacyEnhanceIsHiddenAndNoop();
  testFormantSmoothingMovesDescriptor();
  testHighFormantBandsFadeBeforeNyquist();
  testBlockRateCoefficientTimebase();
  testLevelControlRampsAcrossBlock();
  testMonoDoesNotRetainRightLevelGain();
  testSilentWetDoesNotChargeMakeup();
  testMotionStaysFinite();
  testMetersRespondToModulator();
  std::cout << "vocoder tests passed\n";
  return 0;
}
