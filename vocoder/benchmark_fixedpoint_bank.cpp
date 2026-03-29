#include "fixed_biquad.h"
#include "host_plugin.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

struct FloatBankState {
  float anY1[2][kVocoderMaxBands];
  float anY2[2][kVocoderMaxBands];
  float syY1[2][kVocoderMaxBands];
  float syY2[2][kVocoderMaxBands];
  float modX1[2];
  float modX2[2];
  float carX1[2];
  float carX2[2];
};

struct FixedBankState {
  int32_t anY1[2][kVocoderMaxBands];
  int32_t anY2[2][kVocoderMaxBands];
  int32_t syY1[2][kVocoderMaxBands];
  int32_t syY2[2][kVocoderMaxBands];
  int32_t modX1[2];
  int32_t modX2[2];
  int32_t carX1[2];
  int32_t carX2[2];
};

static void fillSignals(std::vector<float> &carL, std::vector<float> &carR,
                        std::vector<float> &modL, std::vector<float> &modR) {
  const int frames = (int)carL.size();
  for (int i = 0; i < frames; ++i) {
    const float t = (float)i / 48000.0f;
    const float amp = 0.35f + 0.25f * sinf(2.0f * 3.14159265359f * 0.41f * t);
    carL[i] = amp * sinf(2.0f * 3.14159265359f * 110.0f * t);
    carR[i] = amp * sinf(2.0f * 3.14159265359f * 111.7f * t);
    modL[i] = 0.65f * sinf(2.0f * 3.14159265359f * 210.0f * t) +
              0.22f * sinf(2.0f * 3.14159265359f * 420.0f * t);
    modR[i] = modL[i];
  }
}

static void prepareDescriptor(VocoderDescriptor &descriptor, int bandCount) {
  HostAlgorithm host = makeHostAlgorithm();
  hostSetParameter(host, kBandCount, (int16_t)bandCount);
  auto *algo = (_vocoderAlgorithm *)host.algorithm;
  while (algo->controls.descriptorDirty) {
    updateControlState(algo);
  }
  descriptor = *algo->descriptor;
}

static double runFloatBank(const VocoderDescriptor &descriptor,
                           const std::vector<float> &carL,
                           const std::vector<float> &carR,
                           const std::vector<float> &modL,
                           const std::vector<float> &modR,
                           std::vector<float> &outL,
                           std::vector<float> &outR) {
  FloatBankState state = {};
  const int frames = (int)carL.size();
  const int bands = descriptor.activeBands;
  outL.assign(frames, 0.0f);
  outR.assign(frames, 0.0f);

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    float wetL = 0.0f;
    float wetR = 0.0f;
    for (int band = 0; band < bands; ++band) {
      const float yaL = descriptor.an_b0[band] * modL[i] +
                        descriptor.an_b2[band] * state.modX2[0] -
                        descriptor.an_a1[band] * state.anY1[0][band] -
                        descriptor.an_a2[band] * state.anY2[0][band];
      state.anY2[0][band] = state.anY1[0][band];
      state.anY1[0][band] = yaL;

      const float yaR = descriptor.an_b0[band] * modR[i] +
                        descriptor.an_b2[band] * state.modX2[1] -
                        descriptor.an_a1[band] * state.anY1[1][band] -
                        descriptor.an_a2[band] * state.anY2[1][band];
      state.anY2[1][band] = state.anY1[1][band];
      state.anY1[1][band] = yaR;

      const float ysL = descriptor.sy_b0[band] * carL[i] +
                        descriptor.sy_b2[band] * state.carX2[0] -
                        descriptor.sy_a1[band] * state.syY1[0][band] -
                        descriptor.sy_a2[band] * state.syY2[0][band];
      state.syY2[0][band] = state.syY1[0][band];
      state.syY1[0][band] = ysL;

      const float ysR = descriptor.sy_b0[band] * carR[i] +
                        descriptor.sy_b2[band] * state.carX2[1] -
                        descriptor.sy_a1[band] * state.syY1[1][band] -
                        descriptor.sy_a2[band] * state.syY2[1][band];
      state.syY2[1][band] = state.syY1[1][band];
      state.syY1[1][band] = ysR;

      wetL += ysL * fabsf(yaL);
      wetR += ysR * fabsf(yaR);
    }

    state.modX2[0] = state.modX1[0];
    state.modX1[0] = modL[i];
    state.modX2[1] = state.modX1[1];
    state.modX1[1] = modR[i];
    state.carX2[0] = state.carX1[0];
    state.carX1[0] = carL[i];
    state.carX2[1] = state.carX1[1];
    state.carX1[1] = carR[i];

    outL[i] = wetL;
    outR[i] = wetR;
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

static double runFixedBank(const VocoderDescriptor &descriptor,
                           const std::vector<float> &carL,
                           const std::vector<float> &carR,
                           const std::vector<float> &modL,
                           const std::vector<float> &modR,
                           std::vector<float> &outL,
                           std::vector<float> &outR) {
  FixedBankState state = {};
  FixedBiquadQ28 analysis[kVocoderMaxBands];
  FixedBiquadQ28 synthesis[kVocoderMaxBands];
  const int bands = descriptor.activeBands;
  const int frames = (int)carL.size();
  outL.assign(frames, 0.0f);
  outR.assign(frames, 0.0f);

  for (int band = 0; band < bands; ++band) {
    analysis[band].b0 = fixedFromFloat(descriptor.an_b0[band]);
    analysis[band].b2 = fixedFromFloat(descriptor.an_b2[band]);
    analysis[band].a1 = fixedFromFloat(descriptor.an_a1[band]);
    analysis[band].a2 = fixedFromFloat(descriptor.an_a2[band]);
    synthesis[band].b0 = fixedFromFloat(descriptor.sy_b0[band]);
    synthesis[band].b2 = fixedFromFloat(descriptor.sy_b2[band]);
    synthesis[band].a1 = fixedFromFloat(descriptor.sy_a1[band]);
    synthesis[band].a2 = fixedFromFloat(descriptor.sy_a2[band]);
  }

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    const int32_t modSampleL = fixedFromFloat(modL[i]);
    const int32_t modSampleR = fixedFromFloat(modR[i]);
    const int32_t carSampleL = fixedFromFloat(carL[i]);
    const int32_t carSampleR = fixedFromFloat(carR[i]);
    int32_t wetL = 0;
    int32_t wetR = 0;

    for (int band = 0; band < bands; ++band) {
      const int32_t yaL =
          fixedRunBiquadQ28(analysis[band], modSampleL, state.modX2[0],
                            state.anY1[0][band], state.anY2[0][band]);
      const int32_t yaR =
          fixedRunBiquadQ28(analysis[band], modSampleR, state.modX2[1],
                            state.anY1[1][band], state.anY2[1][band]);
      const int32_t ysL =
          fixedRunBiquadQ28(synthesis[band], carSampleL, state.carX2[0],
                            state.syY1[0][band], state.syY2[0][band]);
      const int32_t ysR =
          fixedRunBiquadQ28(synthesis[band], carSampleR, state.carX2[1],
                            state.syY1[1][band], state.syY2[1][band]);

      const int32_t envL = yaL < 0 ? -yaL : yaL;
      const int32_t envR = yaR < 0 ? -yaR : yaR;
      wetL = fixedClamp64To32((int64_t)wetL + (int64_t)fixedMulQ28(ysL, envL));
      wetR = fixedClamp64To32((int64_t)wetR + (int64_t)fixedMulQ28(ysR, envR));
    }

    state.modX2[0] = state.modX1[0];
    state.modX1[0] = modSampleL;
    state.modX2[1] = state.modX1[1];
    state.modX1[1] = modSampleR;
    state.carX2[0] = state.carX1[0];
    state.carX1[0] = carSampleL;
    state.carX2[1] = state.carX1[1];
    state.carX1[1] = carSampleR;

    outL[i] = fixedToFloat(wetL);
    outR[i] = fixedToFloat(wetR);
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

static float rmsError(const std::vector<float> &a, const std::vector<float> &b) {
  double sum = 0.0;
  const int count = (int)a.size();
  for (int i = 0; i < count; ++i) {
    const double diff = (double)a[i] - (double)b[i];
    sum += diff * diff;
  }
  return count > 0 ? (float)sqrt(sum / (double)count) : 0.0f;
}

int main() {
  const int seconds = 5;
  const int frames = seconds * 48000;
  const int bandCounts[] = {8, 16, 24, 32, 40};
  std::vector<float> carL(frames), carR(frames), modL(frames), modR(frames);
  fillSignals(carL, carR, modL, modR);

  std::cout << "Fixed-point bank benchmark at 48 kHz, duration " << seconds
            << " s\n";
  std::cout << std::left << std::setw(8) << "Bands" << std::setw(14)
            << "Float ms" << std::setw(14) << "Fixed ms" << std::setw(12)
            << "Speedup" << "RMS err\n";

  for (int bands : bandCounts) {
    VocoderDescriptor descriptor = {};
    prepareDescriptor(descriptor, bands);

    std::vector<float> floatOutL, floatOutR, fixedOutL, fixedOutR;
    const double floatMs =
        runFloatBank(descriptor, carL, carR, modL, modR, floatOutL, floatOutR);
    const double fixedMs =
        runFixedBank(descriptor, carL, carR, modL, modR, fixedOutL, fixedOutR);
    const float err = rmsError(floatOutL, fixedOutL);
    const double speedup = fixedMs > 0.0 ? floatMs / fixedMs : 0.0;

    std::cout << std::left << std::setw(8) << bands << std::setw(14)
              << std::fixed << std::setprecision(3) << floatMs
              << std::setw(14) << fixedMs << std::setw(12)
              << std::setprecision(2) << speedup << std::setprecision(6) << err
              << "\n";
  }

  return 0;
}
