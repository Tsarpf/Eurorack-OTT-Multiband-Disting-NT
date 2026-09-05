#include "../distingnt_api/include/distingnt/api.h"
#include "../distingnt_api/include/distingnt/serialisation.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>
#include <string>

const _NT_globals NT_globals = {
    .sampleRate = 48000,
    .maxFramesPerStep = 24,
    .workBuffer = nullptr,
    .workBufferSizeBytes = 0,
};
uint8_t NT_screen[128 * 64];

static std::vector<std::string> drawnText;
void NT_drawText(int, int, const char *text, int, _NT_textAlignment, _NT_textSize) {
  drawnText.emplace_back(text);
}
void NT_drawShapeI(_NT_shape, int, int, int, int, int) {}
void NT_setParameterFromUi(uint32_t, uint32_t, int16_t) {}
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


#include "vocoder_algo.cpp"
#include "vocoder_ui.cpp"

static void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "TEST FAILED: " << message << "\n";
    std::exit(1);
  }
}

static bool nearlyEqual(float a, float b, float tolerance) {
  return fabsf(a - b) <= tolerance;
}

static bool sameFloatBits(float a, float b) {
  uint32_t aBits = 0, bBits = 0;
  memcpy(&aBits, &a, sizeof(aBits));
  memcpy(&bBits, &b, sizeof(bBits));
  return aBits == bBits;
}

struct HostAlgorithm {
  _NT_algorithm *algorithm;
  std::vector<uint8_t> sram;
  std::vector<uint8_t> dtc;
  int16_t commonValues[16];
  int16_t values[64];

  HostAlgorithm() : algorithm(nullptr), commonValues{}, values{} {}
  HostAlgorithm(const HostAlgorithm &) = delete;
  HostAlgorithm &operator=(const HostAlgorithm &) = delete;
  HostAlgorithm(HostAlgorithm &&other) noexcept
      : algorithm(other.algorithm), sram(std::move(other.sram)),
        dtc(std::move(other.dtc)) {
    std::copy(std::begin(other.commonValues), std::end(other.commonValues),
              commonValues);
    std::copy(std::begin(other.values), std::end(other.values), values);
    if (algorithm) {
      algorithm->vIncludingCommon = commonValues;
      algorithm->v = values;
    }
    other.algorithm = nullptr;
  }
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
    const int remaining = std::min(block, frames - offset);
    for (int i = 0; i < remaining; ++i) {
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
    for (int i = 0; i < remaining; ++i) {
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

static void fillFilterState(BatchBiquadState &filter, float value) {
  for (unsigned index = 0; index < 2 * filter.inst.numStages; ++index) {
    filter.state[index] = value * (index + 1);
  }
}

static void requireFilterState(const BatchBiquadState &filter, float value,
                               const char *message) {
  for (unsigned index = 0; index < 2 * filter.inst.numStages; ++index) {
    require(nearlyEqual(filter.state[index], value * (index + 1), 1.0e-7f),
            message);
  }
}

static void requireOwnedFilterPointers(const _vocoderAlgorithm &algorithm) {
  const auto &state = *algorithm.state;
  for (int channel = 0; channel < 2; ++channel) {
    for (int band = 0; band < algorithm.activeBands; ++band) {
      const auto &analysis = state.anState[channel][band];
      const auto &synthesis = state.syState[channel][band];
      require(analysis.inst.numStages > 1 && synthesis.inst.numStages > 1,
              "filter bank must use the intended multi-stage cascade");
      require(2 * analysis.inst.numStages <=
                  sizeof(analysis.state) / sizeof(analysis.state[0]) &&
                  2 * synthesis.inst.numStages <=
                  sizeof(synthesis.state) / sizeof(synthesis.state[0]),
              "filter stage count exceeds state storage");
      require(analysis.inst.pState == analysis.state &&
                  synthesis.inst.pState == synthesis.state,
              "CMSIS filter points to another channel's state");
      require(analysis.inst.pCoeffs == state.anCoeffs[band].coeffs &&
                  synthesis.inst.pCoeffs == state.syCoeffs[band].coeffs &&
                  analysis.coefficients == &state.anCoeffs[band] &&
                  synthesis.coefficients == &state.syCoeffs[band],
              "CMSIS filter points to temporary or stale coefficients");
    }
  }
}

static void checkCascadeReseatPreservesAllStagesAndGain(BatchBiquadCoeffs coefficients) {
  BatchBiquadState reference = {};
  batchBiquadInit(reference, coefficients);
  const int frames = 53; // crosses the 24-sample scratch chunks and their tail
  float input[frames], discarded[frames];
  for (int index = 0; index < frames; ++index) {
    input[index] = 0.4f * sinf(0.17f * index) + 0.2f * cosf(0.43f * index);
  }
  batchBiquadProcess(coefficients, reference, input, discarded, frames);
  BatchBiquadState copied = reference;
  BatchBiquadCoeffs copiedCoefficients = coefficients;
  const std::vector<float> originalState(std::begin(copied.state),
                                        std::end(copied.state));
  batchBiquadReseat(copied, copiedCoefficients);
  require(copied.inst.pState == copied.state &&
              copied.inst.pCoeffs == copiedCoefficients.coeffs &&
              copied.coefficients == &copiedCoefficients,
          "copied cascade did not rebind to its own storage");
  for (unsigned index = 0; index < originalState.size(); ++index) {
    require(copied.state[index] == originalState[index],
            "reseating a running cascade reset one of its stages");
  }

  // A direct filter pass followed by scalar gain smoothing must agree
  // with the combined cascade/gain helper, including gain only after the final
  // filter stage and a partial last scratch-buffer chunk.
  float filtered[frames], accumulated[frames];
  std::fill(std::begin(accumulated), std::end(accumulated), 0.125f);
  batchBiquadProcess(coefficients, reference, input, filtered, frames);
  float gain = 0.2f;
  const float mix = 0.99f;
  batchBiquadProcessAndAccum(copied, input, accumulated, frames, gain,
                             0.9f, mix, 1.0f - mix, 0.7f);
  float expectedGain = 0.2f;
  for (int index = 0; index < frames; ++index) {
    expectedGain = mix * expectedGain + (1.0f - mix) * 0.9f;
    const float expected = 0.125f + filtered[index] * expectedGain * 0.7f;
    require(nearlyEqual(accumulated[index], expected, 1.0e-6f),
            "cascade accumulation disagrees with filtering plus smoothed gain");
  }
  require(nearlyEqual(gain, expectedGain, 1.0e-7f),
          "cascade accumulation advanced gain smoothing the wrong number of times");
  for (unsigned index = 0; index < originalState.size(); ++index) {
    require(nearlyEqual(copied.state[index], reference.state[index], 1.0e-7f),
            "cascade accumulation did not preserve every stage's state");
  }
}

static void testCascadeReseatPreservesAllStagesAndGain() {
  float b0, b2, a1, a2;
  vocoderCalculateBandpass(1300.0f, 12.0f, 48000.0f, b0, b2, a1, a2);
  checkCascadeReseatPreservesAllStagesAndGain(batchBiquadFromDF1(b0, b2, a1, a2));
  checkCascadeReseatPreservesAllStagesAndGain(calculateCascade(1300.0f, 12.0f, 48000.0f));
}

static void testEnvelopeOnlyMatchesBufferedCascade() {
  const float frequencies[] = {20.0f, 100.0f, 1000.0f, 18000.0f,
                               20000.0f};
  const float qValues[] = {0.5f, 6.457f, 120.0f};
  const int blockSizes[] = {1, 7, 24, 25, 53, 96};
  uint32_t seed = 0x12345678u;

  for (float frequency : frequencies) {
    for (float q : qValues) {
      const BatchBiquadCoeffs coefficients =
          calculateCascade(frequency, q, 48000.0f);
      for (int blockSize : blockSizes) {
        BatchBiquadState buffered = {}, fused = {}, retained = {};
        batchBiquadInit(buffered, coefficients);
        batchBiquadInit(fused, coefficients);
        batchBiquadInit(retained, coefficients);
        for (int index = 0; index < 2 * kVocoderFilterStages; ++index) {
          seed = 1664525u * seed + 1013904223u;
          const float initial =
              (float)(int32_t)(seed >> 8) * (0.02f / 8388608.0f);
          buffered.state[index] = initial;
          fused.state[index] = initial;
          retained.state[index] = initial;
        }

        std::vector<float> input(blockSize), output(blockSize);
        for (int index = 0; index < blockSize; ++index) {
          seed = 1664525u * seed + 1013904223u;
          input[index] =
              (float)(int32_t)(seed >> 8) * (0.7f / 8388608.0f);
        }

        const float bufferedPeak = batchBiquadProcessWithEnvelope(
            coefficients, buffered, input.data(), output.data(), blockSize);
        const float fusedPeak = batchBiquadEnvelopeOnly(
            coefficients, fused, input.data(), blockSize);
        std::vector<float> retainedOutput(blockSize);
        float retainedPower = 0.0f, referencePower = 0.0f;
        const float retainedPeak = batchBiquadEnvelope<true, true>(
            coefficients, retained, input.data(), blockSize, retainedOutput.data(), &retainedPower);
        for (float sample : output) referencePower += sample * sample;
        referencePower /= blockSize;
        require(nearlyEqual(retainedPower, referencePower, 1.0e-6f * fmaxf(referencePower, 1.0f)),
                "Fused carrier power must match buffered RMS detection");
        require(sameFloatBits(bufferedPeak, retainedPeak) &&
                    memcmp(output.data(), retainedOutput.data(), blockSize * sizeof(float)) == 0 &&
                    memcmp(buffered.state, retained.state, sizeof(buffered.state)) == 0,
                "retaining shared filter output must preserve samples, peak and history");
        require(sameFloatBits(bufferedPeak, fusedPeak),
                "fused analysis changed the envelope peak");
        require(memcmp(buffered.state, fused.state, sizeof(buffered.state)) == 0,
                "fused analysis changed filter state");
      }
    }
  }
}

// Exercise live coefficient changes and full modular transients against the
// buffered realization. Equal state is essential for smooth control motion.
static void testFusedSynthesisMatchesBufferedMotion() {
  const float frequencies[] = {20.0f, 62.5f, 500.0f, 20000.0f, 22678.6f, 23520.0f};
  const float qs[] = {120.0f, 6.0f, 0.23355f};
  const int sizes[] = {1, 7, 24, 25, 53, 96};
  uint32_t seed = 37;
  for (int size : sizes) {
    BatchBiquadCoeffs c = calculateCascade(1000.0f, 6.0f, 48000.0f);
    BatchBiquadState buffered = {}, fused = {}, retained = {};
    batchBiquadInit(buffered, c);
    batchBiquadInit(fused, c);
    float referenceGain = 0.0f, fusedGain = 0.0f;
    for (float frequency : frequencies) for (float q : qs) {
      c = calculateCascade(frequency, q, 48000.0f);
      std::vector<float> input(size), expected(size, 0.25f), actual(size, 0.25f);
      for (float &sample : input) {
        seed = 1664525u * seed + 1013904223u;
        sample = ((float)(seed >> 8) / 8388608.0f - 1.0f) * 40.0f;
      }
      float scratch[24];
      for (int offset = 0; offset < size; offset += 24) {
        const int count = std::min(24, size - offset);
        batchBiquadProcess(c, buffered, input.data() + offset, scratch, count);
        for (int i = 0; i < count; ++i) {
          referenceGain = 0.99f * referenceGain + 0.01f * 0.75f;
          expected[offset + i] += scratch[i] * referenceGain * -0.5f;
        }
      }
      batchBiquadProcessAndAccum(fused, input.data(), actual.data(), size,
                                 fusedGain, 0.75f, 0.99f, 0.01f, -0.5f);
      require(memcmp(buffered.state, fused.state, sizeof(buffered.state)) == 0,
              "fused synthesis changed filter history during control motion");
      require(sameFloatBits(referenceGain, fusedGain), "fused gain history changed");
      for (int i = 0; i < size; ++i)
        require(std::isfinite(actual[i]) &&
                    fabsf(expected[i] - actual[i]) <= 1e-6f * (1.0f + fabsf(expected[i])),
                "fused synthesis differs from buffered response at an extreme");
    }
  }
}

static void testCascadeCenterGainAtDifficultFrequencies() {
  struct Case { float frequency, q; int seconds; };
  const Case cases[] = {{20.0f, 120.0f, 30}, {60.0f, 40.0f, 5},
                        {1000.0f, 6.0f, 1}, {18000.0f, 0.5f, 1}};
  for (const auto &test : cases) {
    BatchBiquadCoeffs coefficients = calculateCascade(test.frequency, test.q, 48000.0f);
    BatchBiquadState state = {};
    batchBiquadInit(state, coefficients);
    const int frames = test.seconds * 48000;
    const int measureStart = frames - 24000;
    double inputPower = 0.0, outputPower = 0.0;
    for (int offset = 0; offset < frames; offset += 24) {
      float input[24], output[24];
      for (int index = 0; index < 24; ++index) {
        input[index] = 0.25f * (float)sin(2.0 * 3.141592653589793 *
            test.frequency * (offset + index) / 48000.0);
      }
      batchBiquadProcess(coefficients, state, input, output, 24);
      for (int index = 0; index < 24; ++index) {
        require(std::isfinite(output[index]),
                "single-band precision probe became non-finite");
        if (offset + index >= measureStart) {
          inputPower += (double)input[index] * input[index];
          outputPower += (double)output[index] * output[index];
        }
      }
    }
    const double gainDb = 10.0 * log10(outputPower / inputPower);
    if (!(fabs(gainDb) < 0.5)) {
      std::cerr << "Center-gain error at " << test.frequency << " Hz, Q="
                << test.q << ": " << gainDb << " dB\n";
    }
    require(fabs(gainDb) < 0.5,
            "unity-center filter lost accuracy at low frequency/high Q or near Nyquist");
  }
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
  require(std::isfinite(algo->descriptor->synthesisQ) &&
              algo->descriptor->synthesisQ > 0.0f,
          "synthesis Q must be finite and positive");
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
  for (int band = 0; band < algorithm->activeBands; ++band) {
    fillFilterState(state.anState[1][band], 0.75f);
    fillFilterState(state.syState[1][band], -0.5f);
  }
  state.env[1][0] = state.env[0][0] + 0.25f;
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
  requireOwnedFilterPointers(*algorithm);
  for (int i = 0; i < block; ++i) {
    require(nearlyEqual(stereoBus[outBusL * block + i],
                        stereoBus[(outBusL + 1) * block + i], 1.0e-6f),
            "stale right DSP state leaked into restored stereo output");
  }
  for (int band = 0; band < algorithm->activeBands; ++band) {
    for (unsigned index = 0;
         index < 2 * state.anState[0][band].inst.numStages; ++index) {
      require(nearlyEqual(state.anState[0][band].state[index],
                          state.anState[1][band].state[index], 1.0e-7f),
              "right analysis cascade stage was not resynchronised");
    }
    for (unsigned index = 0;
         index < 2 * state.syState[0][band].inst.numStages; ++index) {
      require(nearlyEqual(state.syState[0][band].state[index],
                          state.syState[1][band].state[index], 1.0e-7f),
              "right synthesis cascade stage was not resynchronised");
    }
    require(nearlyEqual(state.env[0][band], state.env[1][band], 1.0e-7f) &&
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
  fillFilterState(state.anState[0][0], 0.11f);
  fillFilterState(state.anState[1][0], 0.91f);
  state.env[0][0] = 0.12f;
  state.env[1][0] = 0.92f;
  state.gainState[0][0] = 0.14f;
  state.gainState[1][0] = 0.94f;
  state.modDcX1[0] = 0.15f;
  state.modDcX1[1] = 0.95f;
  fillFilterState(state.syState[0][0], 0.21f);
  fillFilterState(state.syState[1][0], 0.81f);
  state.carrierDcX1[0] = 0.22f;
  state.carrierDcX1[1] = 0.82f;
  host.values[kInModulator] = kNT_lastBus;
  factory.parameterChanged(host.algorithm, kInModulator);
  factory.step(host.algorithm, bus.data(), block / 4);
  require(state.carrierStereoWasActive && !state.modulatorStereoWasActive &&
              state.stereoOutputWasActive,
          "mono modulator incorrectly collapsed the stereo carrier output");
  requireOwnedFilterPointers(*algorithm);
  requireFilterState(state.anState[1][0], 0.11f,
                     "mono modulator did not synchronise every analysis stage");
  require(nearlyEqual(state.env[1][0], 0.12f, 1.0e-7f) &&
              nearlyEqual(state.gainState[1][0], 0.14f, 1.0e-7f) &&
              nearlyEqual(state.modDcX1[1], 0.15f, 1.0e-7f),
          "mono modulator did not synchronise analysis/envelope/gain state");
  requireFilterState(state.syState[1][0], 0.81f,
                     "modulator transition overwrote independent synthesis stages");
  require(nearlyEqual(state.carrierDcX1[1], 0.82f, 1.0e-7f),
          "modulator transition overwrote independent carrier state");

  // Restore the modulator, then make only the carrier effectively mono. Its
  // DC/synthesis state must unify without erasing distinct modulator history.
  host.values[kInModulator] = 3;
  factory.parameterChanged(host.algorithm, kInModulator);
  factory.step(host.algorithm, bus.data(), block / 4);
  fillFilterState(state.syState[0][0], 0.31f);
  fillFilterState(state.syState[1][0], 0.71f);
  state.carrierDcY1[0] = 0.32f;
  state.carrierDcY1[1] = 0.72f;
  fillFilterState(state.anState[0][0], 0.41f);
  fillFilterState(state.anState[1][0], 0.61f);
  state.env[0][0] = 0.42f;
  state.env[1][0] = 0.62f;
  host.values[kInCarrier] = kNT_lastBus;
  factory.parameterChanged(host.algorithm, kInCarrier);
  factory.step(host.algorithm, bus.data(), block / 4);
  require(!state.carrierStereoWasActive && state.modulatorStereoWasActive &&
              state.stereoOutputWasActive,
          "mono carrier incorrectly collapsed the stereo modulator output");
  requireOwnedFilterPointers(*algorithm);
  requireFilterState(state.syState[1][0], 0.31f,
                     "mono carrier did not synchronise every synthesis stage");
  require(nearlyEqual(state.carrierDcY1[1], 0.32f, 1.0e-7f),
          "mono carrier did not synchronise synthesis/DC state");
  requireFilterState(state.anState[1][0], 0.61f,
                     "carrier transition overwrote independent analysis stages");
  require(nearlyEqual(state.env[1][0], 0.62f, 1.0e-7f),
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

static void configureProbe(HostAlgorithm &host, int depth, int width = 100) {
  host.values[kBandCount] = 40;
  host.values[kBandWidth] = width;
  host.values[kDepth] = depth;
  host.values[kMinFreq] = 20;
  host.values[kMaxFreq] = 20000;
  host.values[kAttack] = 10;
  host.values[kRelease] = 30;
  for (int parameter : {kBandCount, kBandWidth, kDepth, kMinFreq, kMaxFreq,
                        kAttack, kRelease}) {
    factory.parameterChanged(host.algorithm, parameter);
  }
}

static double rms(const std::vector<float> &audio, int start, int end) {
  require(start >= 0 && end > start && end <= (int)audio.size(),
          "invalid RMS measurement window");
  double power = 0.0;
  for (int index = start; index < end; ++index) {
    require(std::isfinite(audio[index]), "non-finite sample in RMS measurement");
    power += (double)audio[index] * audio[index];
  }
  return sqrt(power / (end - start));
}

static double settledToneRms(int depth, float amplitude, int width = 100) {
  HostAlgorithm host = makeAlgorithm();
  configureProbe(host, depth, width);
  const int frames = 72000;
  std::vector<float> input(frames), output;
  for (int index = 0; index < frames; ++index) {
    input[index] = amplitude * sinf(2.0f * 3.14159265359f * 1000.0f *
                                   index / 48000.0f);
  }
  processAndMeasureAbs(host, input, input, input, input, &output);
  requireOwnedFilterPointers(*(_vocoderAlgorithm *)host.algorithm);
  return rms(output, frames - 12000, frames);
}

static void testSustainedDepthChangesContrast() {
  const double quiet100 = settledToneRms(100, 0.03f);
  const double quiet200 = settledToneRms(200, 0.03f);
  const double loud100 = settledToneRms(100, 0.3f);
  const double loud200 = settledToneRms(200, 0.3f);
  require(quiet100 > 1.0e-9 && loud100 > 1.0e-8 &&
              loud200 > 1.0e-8,
          "Depth comparison requires audible baselines and a surviving loud tone");
  // Complete rejection of the quiet tone is a valid high-Depth behavior.
  const double quietChange = 20.0 * log10(std::max(quiet200 / quiet100, 1.0e-12));
  const double loudChange = 20.0 * log10(loud200 / loud100);
  require(quietChange < -3.0,
          "Depth 200 must change a settled quiet tone, not just its onset");
  require(loudChange - quietChange > 6.0,
          "Depth 200 must increase sustained loud/quiet contrast");

  // Old presets can still contain the former 800% maximum. Loading one must
  // use the supported endpoint rather than an uncontrolled exponent.
  const double legacyDepth = settledToneRms(800, 0.3f);
  require(fabs(legacyDepth - loud200) < loud200 * 1.0e-5,
          "legacy Depth values above 200 must clamp to the supported endpoint");
  std::cout << "Depth 100->200: quiet " << quietChange << " dB, loud "
            << loudChange << " dB\n";
}

static void testDepthZeroRemainsLevelLinear() {
  const double low = settledToneRms(0, 0.001f);
  const double high = settledToneRms(0, 0.01f);
  require(low > 1.0e-10 && high > 1.0e-9,
          "Depth-zero filterbank must produce a measurable tone");
  require(fabs(high / low - 10.0) < 0.01,
          "Depth zero introduced level-dependent automatic makeup below guards");
}

static void testReleaseDoesNotDipAndRecoverSlowly() {
  HostAlgorithm host = makeAlgorithm();
  configureProbe(host, 100);
  const int stepFrame = 72000;
  const int frames = stepFrame + 120000;
  std::vector<float> input(frames), output;
  for (int index = 0; index < frames; ++index) {
    const float amplitude = index < stepFrame ? 0.5f : 0.03f;
    input[index] = amplitude * sinf(2.0f * 3.14159265359f * 1000.0f *
                                   index / 48000.0f);
  }
  processAndMeasureAbs(host, input, input, input, input, &output);
  const double finalLevel = rms(output, frames - 24000, frames);
  require(finalLevel > 1.0e-9, "release reference level is too quiet to measure");
  double minimum = finalLevel;
  // Ignore the first 100 ms, where the filters and the 30 ms release follower
  // legitimately respond. Measure complete 1 kHz cycles in 10 ms windows.
  for (int start = stepFrame + 4800; start + 480 <= frames; start += 480) {
    minimum = std::min(minimum, rms(output, start, start + 480));
  }
  require(minimum > 0.85 * finalLevel,
          "release step undershot its quiet steady level then recovered slowly");
  const double at300ms = rms(output, stepFrame + 14400, stepFrame + 19200);
  require(at300ms < 1.2 * finalLevel,
          "Release 30 ms retained an unrelated long gain-recovery tail");
  std::cout << "Release step minimum relative to final: "
            << 20.0 * log10(minimum / finalLevel) << " dB\n";
}

static void testImpulseProducesResponse() {
  HostAlgorithm host = makeAlgorithm();
  configureProbe(host, 100);
  const int frames = 24000;
  std::vector<float> carL(frames), carR(frames), modL(frames, 0.0f),
      modR(frames, 0.0f);
  modL[4800] = 1.0f;
  modR[4800] = 1.0f;
  for (int i = 0; i < frames; ++i) {
    const float phase = fmodf(110.0f * i / 48000.0f, 1.0f);
    carL[i] = 2.0f * phase - 1.0f;
    carR[i] = carL[i];
  }

  std::vector<float> output;
  processAndMeasureAbs(host, carL, carR, modL, modR, &output);
  require(rms(output, 0, 4800) < 1.0e-10,
          "Depth 100 emitted a carrier before the modulator impulse");
  require(rms(output, 4800, 14400) > 1.0e-9,
          "modulator impulse failed to open the wet carrier path");
}

static void testEnhance() {
  require(parameters[kEnhance].def == 1 && parameters[kEnhance].enumStrings == onOffEnum,
          "Enhance must default on and have valid enum strings");
  bool paged = false;
  for (uint32_t page = 0; page < paramPages.numPages; ++page)
    for (uint8_t i = 0; i < paramPages.pages[page].numParams; ++i)
      paged |= paramPages.pages[page].params[i] == kEnhance;
  require(paged, "Enhance must be accessible");
  double levels[2][2] = {};
  const int frames = 48000;
  for (int enabled = 0; enabled < 2; ++enabled) {
    for (int level = 0; level < 2; ++level) {
      HostAlgorithm host = makeAlgorithm();
      host.values[kEnhance] = enabled;
      host.values[kDepth] = 0;
      host.values[kBandCount] = 40;
      host.values[kBandWidth] = 100;
      for (int p : {kEnhance, kDepth, kBandCount, kBandWidth})
        factory.parameterChanged(host.algorithm, p);
      std::vector<float> input(frames), output;
      for (int i = 0; i < frames; ++i)
        input[i] = (level ? 2.5f : 0.625f) * sinf(2 * 3.14159265359f * 1000 * i / 48000);
      processAndMeasureAbs(host, input, input, input, input, &output);
      levels[enabled][level] = rms(output, frames / 2, frames);
    }
  }
  require(fabs(20 * log10(levels[0][1] / levels[0][0]) - 12.04) < .2,
          "Enhance off must preserve linear Depth-zero behavior");
  require(fabs(20 * log10(levels[1][1] / levels[1][0]) - 6.02) < .5,
          "Enhance on must compress the carrier approximately 2:1");
}

static void testFastEnvelopePower() {
  for (int d = 1; d <= 200; ++d) {
    const auto shape = vocoderMakeEnvelopeShape(float(d));
    for (int step = -120; step <= 120; ++step) {
      const float x = powf(10.0f, step / 10.0f);
      float reference;
      if (d < 100) reference = powf(x + .56f * (1 - shape.depth), shape.lowerExponent);
      else if (d == 100) reference = x;
      else reference = 5.5f * powf(fmaxf(0, 1 - shape.depth + shape.depth * powf(x / 5.5f, 2.f/7.f)), 3.5f);
      const float actual = vocoderEnvelopeDepthGain(shape, x, 1);
      require(fabsf(actual - reference) <= 0.0002f * fmaxf(reference, 1.0e-4f),
              "Fast Depth gain exceeds approximation error budget");
    }
  }
}

static void testFormantSmoothingMovesDescriptor() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  rebuildDescriptor(algo);
  algo->bankInitialized = true;
  const float before = algo->descriptor->synthesisFreq[4];

  host.values[kFormant] = 120;
  factory.parameterChanged(host.algorithm, kFormant);
  updateControlState(algo);
  require(algo->descriptor->synthesisFreq[4] == before,
          "an incomplete bank must not replace the live descriptor");
  require(algo->pendingBand == 1, "control tick must build only one band pair");
  for (int tick = 1; tick < algo->activeBands; ++tick) updateControlState(algo);
  require(!algo->buildingDescriptor, "bank must finish within its band count");
  require(algo->descriptor->synthesisFreq[4] > before,
          "completed formant bank must raise synthesis frequency");
  require(algo->controls.synthesisCoeffSmoothing,
          "completed bank must smoothly approach new synthesis coefficients");
}

static void testSupportedRangesAndWidthMotion() {
  require(parameters[kBandWidth].min == 0 && parameters[kBandWidth].max == 200,
          "Width must expose its full 0..200 percent range");
  require(parameters[kDepth].min == 0 && parameters[kDepth].max == 200,
          "Depth must expose the supported contrast range");
  require(parameters[kFormant].min == -360 && parameters[kFormant].max == 360 &&
              parameters[kFormant].scaling == kNT_scaling10,
          "Formant must expose plus/minus 36 semitones");
  require(parameters[kMinFreq].min == 20 && parameters[kMaxFreq].max == 20000,
          "filterbank frequency limits must include 20 Hz..20 kHz");

  HostAlgorithm host = makeAlgorithm();
  configureProbe(host, 0);
  auto *algorithm = (_vocoderAlgorithm *)host.algorithm;
  float previousQ = 1.0e9f;
  BatchBiquadCoeffs previousCoefficients = {};
  for (int width : {0, 25, 50, 85, 100, 150, 200}) {
    algorithm->controls.currentBandwidth = (float)width;
    rebuildDescriptor(algorithm);
    const auto &descriptor = *algorithm->descriptor;
    require(nearlyEqual(descriptor.analysisFreq[0], 20.0f, 0.01f) &&
                nearlyEqual(descriptor.analysisFreq[39], 20000.0f, 2.0f),
            "extended filterbank range was silently clamped");
    require(std::isfinite(descriptor.synthesisQ) &&
                descriptor.synthesisQ > 0.0f && descriptor.synthesisQ < previousQ,
            "increasing Width must continue widening synthesis filters");
    const auto &coefficients = descriptor.synthesisCoeffs[21];
    if (width) {
      float maximumChange = 0.0f;
      for (unsigned index = 0; index < sizeof(coefficients.coeffs) / sizeof(float);
           ++index) {
        maximumChange = std::max(maximumChange,
            fabsf(coefficients.coeffs[index] - previousCoefficients.coeffs[index]));
      }
      for (unsigned index = 0; index < sizeof(coefficients.svf) / sizeof(float);
           ++index) {
        maximumChange = std::max(maximumChange,
            fabsf(coefficients.svf[index] - previousCoefficients.svf[index]));
      }
      require(maximumChange > 1.0e-6f,
              "Width changes stopped reaching the synthesis coefficients");
    }
    previousQ = descriptor.synthesisQ;
    previousCoefficients = coefficients;
    requireOwnedFilterPointers(*algorithm);
  }
}

static void testExtremeFormantBandsFadeAtBothEdges() {
  HostAlgorithm host = makeAlgorithm();
  configureProbe(host, 100);
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  const float ceiling = 0.49f * 48000.0f;
  for (int formant : {-360, 360}) {
    host.values[kFormant] = formant;
    factory.parameterChanged(host.algorithm, kFormant);
    for (int iteration = 0; iteration < 100; ++iteration) {
      float bus[24 * kNT_lastBus] = {};
      factory.step(host.algorithm, bus, 6);
    }
    const auto &descriptor = *algo->descriptor;
    require(nearlyEqual(algo->controls.currentFormant, (float)formant, 0.01f),
            "extended formant endpoint was not reached");
    const float ratio = powf(2.0f, formant / 120.0f);
    bool foundFadedBand = false;
    for (int band = 0; band < descriptor.activeBands; ++band) {
      const float shifted = descriptor.analysisFreq[band] * ratio;
      require(descriptor.synthesisFreq[band] >= 20.0f - 0.01f &&
                  descriptor.synthesisFreq[band] <= ceiling + 0.01f,
              "formant shift put a synthesis filter beyond the safe bounds");
      if (shifted >= ceiling || shifted < 10.0f) {
        foundFadedBand = true;
        require(fabsf(descriptor.synthesisBandGain[band]) < 0.001f,
                "out-of-range formant bands must fade instead of piling up");
      } else if (shifted > 40.0f && shifted < 10000.0f) {
        require(nearlyEqual(descriptor.synthesisFreq[band], shifted, shifted * 0.001f),
                "formant shift uses the wrong semitone conversion");
      }
    }
    require(foundFadedBand, "extreme formant test did not reach a bank edge");
    requireOwnedFilterPointers(*algo);
  }
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
                      vocoderMixCoeffFromSeconds(blockRate24, 0.02f),
                      1.0e-6f),
          "synthesis coefficient smoothing must use the block rate");
  require(nearlyEqual(block24.synthesisScalarMix,
                      block24.synthesisCoeffMix, 1.0e-7f),
          "synthesis scalar smoothing must share the block timebase");
  require(nearlyEqual(block24.guardReleaseMix,
                      vocoderMixCoeffFromSeconds(blockRate24, 0.05f),
                      1.0e-6f),
          "output guard smoothing must use the block rate");

  computeBlockCoeffs(algo, 12, 48000.0f);
  require(nearlyEqual(block24.attackMix, coeffs.attackMix, 1.0e-7f) &&
              nearlyEqual(block24.releaseMix, coeffs.releaseMix, 1.0e-7f) &&
              nearlyEqual(block24.enhancePowerMix, coeffs.enhancePowerMix, 1.0e-7f),
          "Envelope coefficients must retain a fixed 24-sample timebase");
  require(nearlyEqual(block24.synthesisCoeffMix,
                      coeffs.synthesisCoeffMix * coeffs.synthesisCoeffMix,
                      2.0e-6f),
          "synthesis smoothing time must be invariant to block size");
  require(nearlyEqual(block24.guardReleaseMix,
                      coeffs.guardReleaseMix * coeffs.guardReleaseMix,
                      2.0e-6f),
          "guard smoothing time must be invariant to block size");
}

static void testOutputGuardRampsAcrossBlock() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  VocoderDSPState &state = *algo->state;
  const int numBuses = 28;

  state.outputGuard[0] = 0.5f;
  state.outputGuardTarget[0] = 0.75f;

  const int firstBlock = 12;
  float firstBus[firstBlock * numBuses] = {};
  factory.step(host.algorithm, firstBus, firstBlock / 4);
  require(nearlyEqual(state.outputGuard[0], 0.75f, 1.0e-6f),
          "guard ramp must reach target on a 12-sample block");

  // Phase 5 produced the next targets. A different-sized callback must use its
  // own N when consuming them, not the preceding block's N.
  const float nextGuardTarget = state.outputGuardTarget[0];
  const int secondBlock = 24;
  float secondBus[secondBlock * numBuses] = {};
  factory.step(host.algorithm, secondBus, secondBlock / 4);
  require(nearlyEqual(state.outputGuard[0], nextGuardTarget, 1.0e-6f),
          "guard ramp must reach target after a block-size change");
}

static void testMonoDoesNotRetainRightGuardGain() {
  HostAlgorithm host = makeAlgorithm();
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  VocoderDSPState &state = *algo->state;
  const int block = 24;
  const int numBuses = 28;

  state.outputGuard[1] = 0.25f;
  state.outputGuardTarget[1] = 0.25f;
  float monoBus[block * numBuses] = {};
  factory.step(host.algorithm, monoBus, block / 4);
  require(nearlyEqual(state.outputGuard[1], state.outputGuard[0], 1.0e-7f) &&
              nearlyEqual(state.outputGuardTarget[1],
                          state.outputGuardTarget[0], 1.0e-7f),
          "mono mode must discard stale right-channel guard gain");

  host.values[kCarrierStereo] = 1;
  const float currentGuard = state.outputGuard[0];
  float stereoBus[block * numBuses] = {};
  factory.step(host.algorithm, stereoBus, block / 4);
  require(state.outputGuard[1] > currentGuard - 0.01f,
          "stereo re-enable must not restore stale right-channel guard gain");
}

static void testFullRangeMotionStaysFiniteAndRecovers(bool enhance = false) {
  HostAlgorithm host = makeAlgorithm();
  configureProbe(host, 200, 0);
  host.values[kEnhance] = enhance;
  factory.parameterChanged(host.algorithm, kEnhance);
  host.values[kCarrierStereo] = 1;
  host.values[kModulatorStereo] = 1;
  factory.parameterChanged(host.algorithm, kCarrierStereo);
  factory.parameterChanged(host.algorithm, kModulatorStereo);
  auto *algorithm = (_vocoderAlgorithm *)host.algorithm;
  const int block = 24;
  const int frames = 48000;
  for (int offset = 0; offset < frames; offset += block) {
    // Hold the narrow endpoint first, then jump across the full range with
    // independent stereo input and periodic band-count rebuilds.
    if (offset >= 9600 && offset % 240 == 0) {
      host.values[kFormant] = (offset / 240) % 2 ? -360 : 360;
      host.values[kBandWidth] = (offset / 480) % 2 ? 0 : 200;
      factory.parameterChanged(host.algorithm, kFormant);
      factory.parameterChanged(host.algorithm, kBandWidth);
    }
    if (offset % 6000 == 0) {
      host.values[kBandCount] = (offset / 6000) % 2 ? 4 : 40;
      factory.parameterChanged(host.algorithm, kBandCount);
    }
    float bus[block * kNT_lastBus] = {};
    for (int index = 0; index < block; ++index) {
      const float phase = 2.0f * 3.14159265359f * (offset + index) / 48000.0f;
      bus[index] = 0.6f * sinf(23.0f * phase) + 0.2f * sinf(1700.0f * phase);
      bus[block + index] = 0.6f * sinf(37.0f * phase) + 0.2f * sinf(6700.0f * phase);
      bus[2 * block + index] = 0.4f * sinf(120.0f * phase) + 0.3f * sinf(2700.0f * phase);
      bus[3 * block + index] = 0.5f * sinf(61.0f * phase) + 0.3f * sinf(5700.0f * phase);
    }
    factory.step(host.algorithm, bus, block / 4);
    requireOwnedFilterPointers(*algorithm);
    for (int index = 0; index < block; ++index) {
      for (int channel = 0; channel < 2; ++channel) {
        const float output = bus[(12 + channel) * block + index];
        require(std::isfinite(output) && fabsf(output) <= 10.001f,
                "full-range motion exceeded output protection or became non-finite");
      }
    }
    for (int channel = 0; channel < 2; ++channel) {
      for (int band = 0; band < algorithm->activeBands; ++band) {
        const auto &analysis = algorithm->state->anState[channel][band];
        const auto &synthesis = algorithm->state->syState[channel][band];
        for (unsigned index = 0; index < 2 * analysis.inst.numStages; ++index) {
          require(std::isfinite(analysis.state[index]),
                  "analysis cascade became unstable behind the output guard");
        }
        for (unsigned index = 0; index < 2 * synthesis.inst.numStages; ++index) {
          require(std::isfinite(synthesis.state[index]),
                  "synthesis cascade became unstable behind the output guard");
        }
      }
    }
  }

  // Return to a broad, unshifted bank and silence. All stages, including ones
  // hidden by temporary band-count changes, must shed the stressed history.
  configureProbe(host, 100, 100);
  host.values[kFormant] = 0;
  factory.parameterChanged(host.algorithm, kFormant);
  std::vector<float> silence(144000, 0.0f), outputL, outputR;
  processAndMeasureAbs(host, silence, silence, silence, silence, &outputL, &outputR);
  require(rms(outputL, 132000, 144000) < 1.0e-5 &&
              rms(outputR, 132000, 144000) < 1.0e-5,
          "filterbank failed to return to silence after extreme control motion");
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

static void testCoefficientWorkBudget() {
  HostAlgorithm host = makeAlgorithm();
  auto *a = (_vocoderAlgorithm *)host.algorithm;
  host.values[kBandCount] = 40;
  host.values[kBandWidth] = 200;
  factory.parameterChanged(host.algorithm, kBandCount);
  factory.parameterChanged(host.algorithm, kBandWidth);
  float bus[4 * kNT_lastBus] = {};
  for (int callback = 0; callback < 240; ++callback) {
    factory.step(host.algorithm, bus, 1);
    require(a->pendingBand == (callback + 1) / 6,
            "small callbacks exceeded one coefficient pair per 24 frames");
    if (callback < 239)
      require(a->activeBands != 40, "partially built bank was published");
  }
  require(a->activeBands == 40 && !a->buildingDescriptor,
          "complete 40-band bank did not publish within 20 ms");
  const VocoderDescriptor published = *a->descriptor;
  rebuildDescriptor(a);
  for (int band = 0; band < 40; ++band) {
    require(memcmp(published.analysisCoeffs[band].svf,
                   a->descriptor->analysisCoeffs[band].svf,
                   sizeof(published.analysisCoeffs[band].svf)) == 0,
            "incremental analysis changed the final filter shape");
    require(memcmp(published.synthesisCoeffs[band].svf,
                   a->descriptor->synthesisCoeffs[band].svf,
                   sizeof(published.synthesisCoeffs[band].svf)) == 0,
            "incremental synthesis changed the final filter shape");
  }
}

static void testWidthChangeDoesNotDuckSaw() {
  HostAlgorithm host = makeAlgorithm();
  configureProbe(host, 100, 200);
  double referencePower = 0.0, windowPower = 0.0;
  double minimumMovingRms = 1e9;
  const int changeFrame = 72000;
  for (int offset = 0; offset < 96000; offset += 24) {
    if (offset == changeFrame) {
      host.values[kBandWidth] = 100;
      factory.parameterChanged(host.algorithm, kBandWidth);
    }
    float bus[24 * kNT_lastBus] = {};
    for (int i = 0; i < 24; ++i) {
      const float phase = fmodf((offset + i) * 110.0f / 48000.0f, 1.0f);
      bus[i] = bus[48 + i] = 4.0f * (2.0f * phase - 1.0f);
    }
    factory.step(host.algorithm, bus, 6);
    for (int i = 0; i < 24; ++i) {
      const double power = (double)bus[288 + i] * bus[288 + i];
      if (offset >= changeFrame - 9600 && offset < changeFrame)
        referencePower += power;
      if (offset >= changeFrame) windowPower += power;
    }
    if (offset >= changeFrame && (offset + 24 - changeFrame) % 960 == 0) {
      minimumMovingRms = std::min(minimumMovingRms, sqrt(windowPower / 960));
      windowPower = 0;
    }
  }
  const double referenceRms = sqrt(referencePower / 9600);
  require(referenceRms > 0.1, "Width transition probe must produce audible audio");
  require(minimumMovingRms > referenceRms * 0.85,
          "Width 200->100 caused a temporary saw level collapse");
}

static void testCoefficientInterpolationFinishes() {
  for (int formant : {-360, 120, 360}) {
    HostAlgorithm host = makeAlgorithm();
    configureProbe(host, 100, 200);
    host.values[kFormant] = formant;
    factory.parameterChanged(host.algorithm, kFormant);
    auto *a = (_vocoderAlgorithm *)host.algorithm;
    for (int i = 0; i < 4000; ++i) {
      float buses[24 * kNT_lastBus] = {};
      factory.step(host.algorithm, buses, 6);
    }
    require(!a->buildingDescriptor && !a->controls.synthesisCoeffSmoothing,
            "Stationary extreme band gains must not interpolate forever");
    for (int band = 0; band < a->activeBands; ++band)
      require(memcmp(a->state->syCoeffs[band].svf,
                     a->descriptor->synthesisCoeffs[band].svf, 8 * sizeof(float)) == 0,
              "Settled filters must reach the exact designed coefficients");
  }
}

static void testShortCallbacksKeepEnvelopeRate() {
  double level[2][2] = {};
  for (int enhanced = 0; enhanced < 2; ++enhanced) {
    for (int shortBlocks = 0; shortBlocks < 2; ++shortBlocks) {
      HostAlgorithm host = makeAlgorithm();
      configureProbe(host, 100, 100);
      host.values[kEnhance] = enhanced;
      factory.parameterChanged(host.algorithm, kEnhance);
      const int count = shortBlocks ? 4 : 24;
      double sum = 0;
      for (int offset = 0; offset < 48000; offset += count) {
        float buses[24 * kNT_lastBus] = {};
        for (int i = 0; i < count; ++i) {
          const float t = (offset + i) / 48000.0f;
          buses[i] = buses[2 * count + i] =
              .2f * sinf(2 * 3.14159265359f * 3000 * t) +
              .1f * sinf(2 * 3.14159265359f * 12000 * t);
        }
        factory.step(host.algorithm, buses, count / 4);
        const auto *a = (_vocoderAlgorithm *)host.algorithm;
        require(a->state->envelopeFrames == (offset + count) % 24,
                "Envelope control work must accumulate short callbacks");
        if (offset >= 24000)
          for (int i = 0; i < count; ++i)
            sum += double(buses[12 * count + i]) * buses[12 * count + i];
      }
      level[enhanced][shortBlocks] = sqrt(sum / 24000);
    }
    require(fabs(20 * log10(level[enhanced][0] / level[enhanced][1])) < .1,
            "Short callbacks must preserve the measured sustained envelope level");
  }
}

static void testSignedDisplay() {
  HostAlgorithm host = makeAlgorithm();
  auto *a = (_vocoderAlgorithm *)host.algorithm;
  host.values[kFormant] = -123;
  a->uiOutputGainDisplay = -60;
  drawnText.clear();
  draw(host.algorithm);
  require(std::find(drawnText.begin(), drawnText.end(), "-123") != drawnText.end(),
          "Formant must use the original integer display");
  require(std::find(drawnText.begin(), drawnText.end(), "GAIN -60") != drawnText.end(),
          "Gain must use the original integer display");
}

int main() {
  testSignedDisplay();
  testShortCallbacksKeepEnvelopeRate();
  testCoefficientInterpolationFinishes();
  testWidthChangeDoesNotDuckSaw();
  testCoefficientWorkBudget();
  testCascadeReseatPreservesAllStagesAndGain();
  testEnvelopeOnlyMatchesBufferedCascade();
  testFusedSynthesisMatchesBufferedMotion();
  testCascadeCenterGainAtDifficultFrequencies();
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
  testSustainedDepthChangesContrast();
  testDepthZeroRemainsLevelLinear();
  testReleaseDoesNotDipAndRecoverSlowly();
  testImpulseProducesResponse();
  testEnhance();
  testFastEnvelopePower();
  testFormantSmoothingMovesDescriptor();
  testSupportedRangesAndWidthMotion();
  testExtremeFormantBandsFadeAtBothEdges();
  testBlockRateCoefficientTimebase();
  testOutputGuardRampsAcrossBlock();
  testMonoDoesNotRetainRightGuardGain();
  testFullRangeMotionStaysFiniteAndRecovers();
  testFullRangeMotionStaysFiniteAndRecovers(true);
  testMetersRespondToModulator();
  std::cout << "vocoder tests passed\n";
  return 0;
}
