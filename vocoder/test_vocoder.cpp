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
  const int numBuses = 28;
  const int outBusL = host.values[kOut] - 1;
  const int outBusR = outBusL + 1;
  for (int offset = 0; offset < frames; offset += block) {
    float bus[block * numBuses];
    memset(bus, 0, sizeof(bus));
    for (int i = 0; i < block; ++i) {
      bus[0 * block + i] = carL[offset + i];
      bus[1 * block + i] = carR[offset + i];
      bus[2 * block + i] = modL[offset + i];
      bus[3 * block + i] = modR[offset + i];
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

static void testEnhanceIsNoop() {
  // Enhance computation was removed (task E); the parameter is kept for
  // UI/serialisation compatibility but must have no effect on audio output.
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
  require(diff < 1.0e-4f, "enhance should be a no-op after removal");
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
  for (int offset = 0; offset < frames; offset += block) {
    host.values[kFormant] = (int16_t)(120.0f * sinf(offset / 240.0f));
    host.values[kBandWidth] = (int16_t)(50.0f + 40.0f * sinf(offset / 180.0f));
    factory.parameterChanged(host.algorithm, kFormant);
    factory.parameterChanged(host.algorithm, kBandWidth);

    float bus[block * 4];
    for (int i = 0; i < block; ++i) {
      bus[0 * block + i] = carL[offset + i];
      bus[1 * block + i] = carR[offset + i];
      bus[2 * block + i] = modL[offset + i];
      bus[3 * block + i] = modR[offset + i];
    }

    factory.step(host.algorithm, bus, block / 4);
    for (int i = 0; i < block * 4; ++i) {
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
  testImpulseProducesResponse();
  testEnhanceIsNoop();
  testFormantSmoothingMovesDescriptor();
  testMotionStaysFinite();
  testMetersRespondToModulator();
  std::cout << "vocoder tests passed\n";
  return 0;
}
