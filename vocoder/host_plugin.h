#ifndef VOCODER_HOST_PLUGIN_H
#define VOCODER_HOST_PLUGIN_H

#include "../distingnt_api/include/distingnt/api.h"
#include "../distingnt_api/include/distingnt/serialisation.h"
#include <cstdint>
#include <cstring>
#include <vector>

const _NT_globals NT_globals = {
    .sampleRate = 48000,
    .maxFramesPerStep = 24,
    .workBuffer = nullptr,
    .workBufferSizeBytes = 0,
};

uint8_t NT_screen[128 * 64];

inline void NT_drawText(int, int, const char *, int, _NT_textAlignment,
                        _NT_textSize) {}
inline void NT_drawShapeI(_NT_shape, int, int, int, int, int) {}
inline void NT_setParameterFromUi(int, int, int) {}
inline int NT_algorithmIndex(_NT_algorithm *) { return 0; }
inline uint32_t NT_parameterOffset(void) { return 0; }
inline uint32_t NT_getCpuCycleCount(void) { return 0; }

inline bool draw(_NT_algorithm *) { return false; }
inline uint32_t hasCustomUi(_NT_algorithm *) { return 0; }
inline void customUi(_NT_algorithm *, const _NT_uiData &) {}
inline void setupUi(_NT_algorithm *, _NT_float3 &) {}

inline void _NT_jsonStream::addMemberName(const char *) {}
inline void _NT_jsonStream::addNumber(int) {}
inline void _NT_jsonStream::addNumber(float) {}
inline bool _NT_jsonParse::numberOfObjectMembers(int &num) {
  num = 0;
  return true;
}
inline bool _NT_jsonParse::matchName(const char *) { return false; }
inline bool _NT_jsonParse::number(int &) { return false; }
inline bool _NT_jsonParse::skipMember(void) { return true; }

#include "vocoder_algo.cpp"

struct HostAlgorithm {
  _NT_algorithm *algorithm = nullptr;
  std::vector<uint8_t> sram;
  std::vector<uint8_t> dtc;
  int16_t commonValues[16];
  int16_t values[64];
};

inline HostAlgorithm makeHostAlgorithm() {
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
  host.values[kBandCount] = 16;
  host.values[kBandWidth] = 50;
  host.values[kDepth] = 70;
  host.values[kFormant] = 0;
  host.values[kMinFreq] = 30;
  host.values[kMaxFreq] = 18000;
  host.values[kAttack] = 10;
  host.values[kRelease] = 120;
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

inline void hostSetParameter(HostAlgorithm &host, int index, int16_t value) {
  host.values[index] = value;
  factory.parameterChanged(host.algorithm, index);
}

template <typename Automation>
inline void renderHostAlgorithm(HostAlgorithm &host, const float *carrierL,
                                const float *carrierR, const float *modL,
                                const float *modR, int frames,
                                std::vector<float> &outL,
                                std::vector<float> &outR,
                                Automation automation) {
  const int block = 24;
  outL.clear();
  outR.clear();
  outL.reserve(frames);
  outR.reserve(frames);
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
    automation(host, offset, block);
    float bus[block * numBuses];
    memset(bus, 0, sizeof(bus));
    for (int i = 0; i < block; ++i) {
      bus[carrierBusL * block + i] = carrierL[offset + i];
      if (carrierBusR != carrierBusL) {
        bus[carrierBusR * block + i] = carrierR[offset + i];
      }
      bus[modulatorBusL * block + i] = modL[offset + i];
      if (modulatorBusR != modulatorBusL) {
        bus[modulatorBusR * block + i] = modR[offset + i];
      }
    }
    factory.step(host.algorithm, bus, block / 4);
    for (int i = 0; i < block; ++i) {
      outL.push_back(bus[outBusL * block + i]);
      outR.push_back(bus[outBusR * block + i]);
    }
  }
}

inline void renderHostAlgorithm(HostAlgorithm &host, const float *carrierL,
                                const float *carrierR, const float *modL,
                                const float *modR, int frames,
                                std::vector<float> &outL,
                                std::vector<float> &outR) {
  renderHostAlgorithm(
      host, carrierL, carrierR, modL, modR, frames, outL, outR,
      [](HostAlgorithm &, int, int) {});
}

#endif // VOCODER_HOST_PLUGIN_H
