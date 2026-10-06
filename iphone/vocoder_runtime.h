#ifndef IPHONE_VOCODER_RUNTIME_H
#define IPHONE_VOCODER_RUNTIME_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../vocoder/vocoder_parameters.h"

// The NT implementation currently designs its bank from NT_globals.sampleRate.
// The iPhone audio session and the AU target therefore negotiate 48 kHz.  This
// is also the rate used by the Disting NT USB audio path in the test setup.
static constexpr uint32_t kVocoderRuntimeSampleRate = 48000;
static constexpr int kVocoderRuntimeBlock = 24;

class VocoderRuntime {
public:
  explicit VocoderRuntime(uint32_t sampleRate = kVocoderRuntimeSampleRate);
  ~VocoderRuntime();

  VocoderRuntime(const VocoderRuntime &) = delete;
  VocoderRuntime &operator=(const VocoderRuntime &) = delete;

  bool initialize();
  bool isInitialized() const { return algorithm_ != nullptr; }
  uint32_t sampleRate() const { return sampleRate_; }

  // Process arbitrary interleaved-mono frame counts.  The NT callback accepts
  // four-frame quanta, so the adapter chunks larger host blocks and zero-pads a
  // final short quantum while returning only the requested frames.
  void process(const float *input, float *output, size_t frames);

  // UI/AUv3 changes are published lock-free and applied at the beginning of
  // the next render call. parameterChanged() itself is cheap; the bank rebuild
  // remains incremental inside the DSP callback just as it is on the NT.
  void setParameter(int parameter, int value);
  int parameter(int parameter) const;

private:
  void applyPendingParameters();
  void primeDescriptor();

  uint32_t sampleRate_;
  const struct _NT_factory *factory_;
  struct _NT_algorithm *algorithm_;
  uint8_t *sram_;
  uint8_t *dtc_;
  size_t sramSize_;
  size_t dtcSize_;
  int16_t values_[kNumParams];
  int16_t commonValues_[16];
  std::atomic<int16_t> pendingValues_[kNumParams];
  std::atomic<uint32_t> pendingMask_;
  float bus_[64 * kVocoderRuntimeBlock];
};

#endif // IPHONE_VOCODER_RUNTIME_H
