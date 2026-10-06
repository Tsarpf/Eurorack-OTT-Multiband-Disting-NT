#ifndef IPHONE_VOCODER_AUDIO_ENGINE_H
#define IPHONE_VOCODER_AUDIO_ENGINE_H

#include "vocoder_runtime.h"

#include <cstdint>

typedef struct OpaqueAudioComponentInstance *AudioUnit;

class VocoderAudioEngine {
public:
  VocoderAudioEngine();
  ~VocoderAudioEngine();

  VocoderAudioEngine(const VocoderAudioEngine &) = delete;
  VocoderAudioEngine &operator=(const VocoderAudioEngine &) = delete;

  bool start();
  void stop();
  bool isRunning() const { return running_; }
  const char *routeName() const { return routeName_; }

  void setParameter(int parameter, int value) {
    runtime_.setParameter(parameter, value);
  }
  int parameter(int parameter) const { return runtime_.parameter(parameter); }

private:
  static long renderCallback(void *refCon, unsigned int *actionFlags,
                             const void *timestamp, unsigned int busNumber,
                             unsigned int frameCount, void *ioData);
  long render(unsigned int *actionFlags, const void *timestamp,
              unsigned int busNumber, unsigned int frameCount, void *ioData);

  VocoderRuntime runtime_;
  AudioUnit audioUnit_;
  bool running_;
  char routeName_[128];
  float input_[4096];
  float output_[4096];
};

#endif // IPHONE_VOCODER_AUDIO_ENGINE_H
