#include "../distingnt_api/include/distingnt/api.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
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

bool draw(_NT_algorithm *) { return false; }
uint32_t hasCustomUi(_NT_algorithm *) { return 0; }
void customUi(_NT_algorithm *, const _NT_uiData &) {}
void setupUi(_NT_algorithm *, _NT_float3 &) {}

#include "vocoder_algo.cpp"

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
  host.values[kCarrierStereo] = 1;
  host.values[kInModulator] = 3;
  host.values[kModulatorStereo] = 1;
  host.values[kOut] = 13;
  host.values[kOutMode] = 1;
  host.values[kBandCount] = 8;
  host.values[kBandWidth] = 50;
  host.values[kDepth] = 70;
  host.values[kFormant] = 0;
  host.values[kMinFreq] = 35;
  host.values[kMaxFreq] = 18000;
  host.values[kAttack] = 10;
  host.values[kRelease] = 120;
  host.values[kEnhance] = 1;
  host.values[kWet] = 100;
  host.values[kOutputGain] = 0;

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

static void fillBuffers(std::vector<float> &carL, std::vector<float> &carR,
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

static double runBenchmarkCase(int bandCount, bool movingControls,
                               double seconds) {
  HostAlgorithm host = makeAlgorithm();
  host.values[kBandCount] = (int16_t)bandCount;
  factory.parameterChanged(host.algorithm, kBandCount);

  const int totalFrames = (int)(seconds * 48000.0);
  const int block = 24;
  std::vector<float> carL(totalFrames), carR(totalFrames), modL(totalFrames),
      modR(totalFrames);
  fillBuffers(carL, carR, modL, modR);

  auto start = std::chrono::steady_clock::now();
  for (int offset = 0; offset < totalFrames; offset += block) {
    if (movingControls) {
      host.values[kFormant] = (int16_t)(120.0f * sinf(offset / 2000.0f));
      host.values[kBandWidth] =
          (int16_t)(50.0f + 35.0f * sinf(offset / 1700.0f));
      factory.parameterChanged(host.algorithm, kFormant);
      factory.parameterChanged(host.algorithm, kBandWidth);
    }

    float bus[block * 4];
    for (int i = 0; i < block; ++i) {
      bus[0 * block + i] = carL[offset + i];
      bus[1 * block + i] = carR[offset + i];
      bus[2 * block + i] = modL[offset + i];
      bus[3 * block + i] = modR[offset + i];
    }
    factory.step(host.algorithm, bus, block / 4);
  }
  auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

int main() {
  const double seconds = 5.0;
  const int bandCounts[] = {8, 16, 24, 32, 40};

  std::cout << "Host benchmark at 48 kHz, block size 24, duration "
            << seconds << " s\n";
  std::cout << std::left << std::setw(8) << "Bands" << std::setw(12) << "Case"
            << std::setw(14) << "Elapsed ms" << std::setw(14) << "x realtime"
            << "ms/frame\n";

  for (int bands : bandCounts) {
    for (int mode = 0; mode < 2; ++mode) {
      const bool moving = mode == 1;
      const double elapsedMs = runBenchmarkCase(bands, moving, seconds);
      const double realtime = (seconds * 1000.0) / elapsedMs;
      const double msPerFrame = elapsedMs / (seconds * 48000.0);

      std::cout << std::left << std::setw(8) << bands
                << std::setw(12) << (moving ? "moving" : "static")
                << std::setw(14) << std::fixed << std::setprecision(3)
                << elapsedMs << std::setw(14) << std::fixed
                << std::setprecision(2) << realtime << std::fixed
                << std::setprecision(6) << msPerFrame << "\n";
    }
  }

  return 0;
}
