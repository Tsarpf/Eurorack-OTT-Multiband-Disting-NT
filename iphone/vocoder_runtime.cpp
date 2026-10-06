#include "vocoder_runtime.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "../distingnt_api/include/distingnt/api.h"

namespace {

static int16_t clampParameter(const _NT_parameter &parameter, int value) {
  const int bounded = std::max<int>(parameter.min, std::min<int>(parameter.max, value));
  return static_cast<int16_t>(bounded);
}

} // namespace

VocoderRuntime::VocoderRuntime(uint32_t sampleRate)
    : sampleRate_(sampleRate), factory_(nullptr), algorithm_(nullptr),
      sram_(nullptr), dtc_(nullptr), sramSize_(0), dtcSize_(0), values_{},
      commonValues_{}, pendingValues_{}, pendingMask_(0), bus_{} {}

VocoderRuntime::~VocoderRuntime() {
  std::free(sram_);
  std::free(dtc_);
}

bool VocoderRuntime::initialize() {
  if (algorithm_ != nullptr) {
    return true;
  }
  if (sampleRate_ != kVocoderRuntimeSampleRate) {
    return false;
  }

  factory_ = reinterpret_cast<const _NT_factory *>(
      pluginEntry(kNT_selector_factoryInfo, 0));
  if (factory_ == nullptr || factory_->calculateRequirements == nullptr ||
      factory_->construct == nullptr || factory_->step == nullptr) {
    return false;
  }

  _NT_algorithmRequirements requirements = {};
  factory_->calculateRequirements(requirements, nullptr);
  sramSize_ = requirements.sram;
  dtcSize_ = requirements.dtc;
  if (posix_memalign(reinterpret_cast<void **>(&sram_), alignof(max_align_t),
                     sramSize_) != 0 ||
      posix_memalign(reinterpret_cast<void **>(&dtc_), alignof(max_align_t),
                     dtcSize_) != 0) {
    std::free(sram_);
    std::free(dtc_);
    sram_ = nullptr;
    dtc_ = nullptr;
    return false;
  }
  std::memset(sram_, 0, sramSize_);
  std::memset(dtc_, 0, dtcSize_);
  std::memset(commonValues_, 0, sizeof(commonValues_));

  _NT_algorithmMemoryPtrs pointers = {sram_, nullptr, dtc_, nullptr};
  algorithm_ = factory_->construct(pointers, requirements, nullptr);
  if (algorithm_ == nullptr || algorithm_->parameters == nullptr) {
    std::free(sram_);
    std::free(dtc_);
    sram_ = nullptr;
    dtc_ = nullptr;
    return false;
  }
  algorithm_->vIncludingCommon = commonValues_;
  algorithm_->v = values_;

  for (int parameter = 0; parameter < kNumParams; ++parameter) {
    values_[parameter] = algorithm_->parameters[parameter].def;
  }

  // A single incoming stream is deliberately both carrier and modulator.
  // These defaults favour a full-bandwidth, Ableton-like 40-band vocoder.
  values_[kInCarrier] = 1;
  values_[kCarrierStereo] = 0;
  values_[kInModulator] = 1;
  values_[kModulatorStereo] = 0;
  values_[kOut] = 13;
  values_[kOutMode] = 1;
  values_[kBandCount] = 40;
  values_[kBandWidth] = 100;
  values_[kDepth] = 100;
  values_[kFormant] = 0;
  values_[kMinFreq] = 20;
  values_[kMaxFreq] = 18000;
  values_[kAttack] = 10;
  values_[kRelease] = 30;
  values_[kEnhance] = 1;
  values_[kWet] = 100;
  values_[kPreGain] = 0;

  for (int parameter = 0; parameter < kNumParams; ++parameter) {
    pendingValues_[parameter].store(values_[parameter],
                                    std::memory_order_relaxed);
    factory_->parameterChanged(algorithm_, parameter);
  }
  pendingMask_.store(0, std::memory_order_release);

  // The NT bank builder publishes one band per 24-frame callback. Build the
  // initial bank before audio starts, avoiding a silent first quarter-second
  // while the iPhone audio unit is coming online.
  primeDescriptor();
  return true;
}

void VocoderRuntime::primeDescriptor() {
  std::memset(bus_, 0, sizeof(bus_));
  // 40 bands plus a little settling room for the first descriptor publish.
  for (int block = 0; block < 48; ++block) {
    factory_->step(algorithm_, bus_, kVocoderRuntimeBlock / 4);
    std::memset(bus_, 0, sizeof(bus_));
  }
}

void VocoderRuntime::applyPendingParameters() {
  const uint32_t mask = pendingMask_.exchange(0, std::memory_order_acquire);
  if (mask == 0) {
    return;
  }
  for (int parameter = 0; parameter < kNumParams; ++parameter) {
    if ((mask & (uint32_t(1) << parameter)) == 0) {
      continue;
    }
    values_[parameter] = pendingValues_[parameter].load(std::memory_order_relaxed);
    factory_->parameterChanged(algorithm_, parameter);
  }
}

void VocoderRuntime::setParameter(int parameter, int value) {
  if (!algorithm_ || parameter < 0 || parameter >= kNumParams) {
    return;
  }
  const int16_t bounded = clampParameter(algorithm_->parameters[parameter], value);
  pendingValues_[parameter].store(bounded, std::memory_order_relaxed);
  pendingMask_.fetch_or(uint32_t(1) << parameter, std::memory_order_release);
}

int VocoderRuntime::parameter(int parameter) const {
  if (parameter < 0 || parameter >= kNumParams) {
    return 0;
  }
  return pendingValues_[parameter].load(std::memory_order_relaxed);
}

void VocoderRuntime::process(const float *input, float *output, size_t frames) {
  if (output == nullptr || frames == 0) {
    return;
  }
  if (!algorithm_ || input == nullptr) {
    if (input != output) {
      std::memset(output, 0, frames * sizeof(float));
    }
    return;
  }

  applyPendingParameters();

  size_t offset = 0;
  while (offset < frames) {
    const size_t remaining = frames - offset;
    const int requested = static_cast<int>(
        std::min<size_t>(remaining, kVocoderRuntimeBlock));
    const int count = requested >= 4 ? requested - (requested % 4) : 0;
    const int renderFrames = count > 0 ? count : 4;

    std::memset(bus_, 0, sizeof(bus_));
    float *carrier = bus_ + (values_[kInCarrier] - 1) * renderFrames;
    float *modulator = bus_ + (values_[kInModulator] - 1) * renderFrames;
    float *out = bus_ + (values_[kOut] - 1) * renderFrames;
    for (int frame = 0; frame < renderFrames; ++frame) {
      const float sample = frame < requested ? input[offset + frame] : 0.0f;
      carrier[frame] = sample;
      modulator[frame] = sample;
    }

    factory_->step(algorithm_, bus_, renderFrames / 4);
    const float *rendered = out;
    for (int frame = 0; frame < requested; ++frame) {
      output[offset + frame] = rendered[frame];
    }
    offset += static_cast<size_t>(requested);
  }
}
