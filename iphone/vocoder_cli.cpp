// Small offline/SSH harness for the portable iPhone vocoder runtime.

#include "vocoder_runtime.h"

#include "../vocoder/wav_io.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

static WavData makeFixture() {
  WavData wav;
  wav.sampleRate = static_cast<int>(kVocoderRuntimeSampleRate);
  wav.channels = 1;
  const size_t frames = static_cast<size_t>(wav.sampleRate) * 3;
  wav.samples.resize(frames);
  for (size_t i = 0; i < frames; ++i) {
    const float t = static_cast<float>(i) / wav.sampleRate;
    const float phrase =
        0.5f + 0.5f * std::sin(2.0f * 3.14159265359f * 1.3f * t);
    const float f0 = 115.0f + 20.0f * std::sin(2.0f * 3.14159265359f * 0.37f * t);
    float voice = 0.0f;
    for (int harmonic = 1; harmonic <= 12; ++harmonic) {
      voice += (1.0f / harmonic) *
               std::sin(2.0f * 3.14159265359f * f0 * harmonic * t);
    }
    const float fricative =
        0.025f * std::sin(2.0f * 3.14159265359f * 4200.0f * t) *
        std::sin(2.0f * 3.14159265359f * 0.71f * t);
    wav.samples[i] = 0.08f * phrase * voice + fricative;
  }
  return wav;
}

static std::vector<float> firstChannel(const WavData &wav) {
  if (wav.channels == 1) {
    return wav.samples;
  }
  std::vector<float> mono;
  mono.reserve(wav.samples.size() / static_cast<size_t>(wav.channels));
  for (size_t frame = 0; frame < wav.samples.size();
       frame += static_cast<size_t>(wav.channels)) {
    mono.push_back(wav.samples[frame]);
  }
  return mono;
}

static void printStats(const std::vector<float> &samples) {
  double sum = 0.0;
  float peak = 0.0f;
  size_t finite = 0;
  for (float sample : samples) {
    if (std::isfinite(sample)) {
      ++finite;
    }
    sum += static_cast<double>(sample) * sample;
    peak = std::fmax(peak, std::fabs(sample));
  }
  const double rms = samples.empty() ? 0.0 : std::sqrt(sum / samples.size());
  std::cout << "frames=" << samples.size() << " rms=" << rms
            << " peak=" << peak << " finite=" << finite << "/"
            << samples.size() << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const std::string inputPath = argc > 1 ? argv[1] : "";
    const std::string outputPath = argc > 2 ? argv[2] : "/tmp/vocoder-iphone-test.wav";
    WavData input = inputPath.empty() ? makeFixture() : readWavFile(inputPath);
    if (input.sampleRate != static_cast<int>(kVocoderRuntimeSampleRate)) {
      std::cerr << "input must be 48000 Hz (the runtime follows the NT bank rate)\n";
      return 2;
    }

    const std::vector<float> mono = firstChannel(input);
    std::vector<float> rendered(mono.size(), 0.0f);
    VocoderRuntime runtime(kVocoderRuntimeSampleRate);
    if (!runtime.initialize()) {
      std::cerr << "failed to initialise vocoder runtime\n";
      return 3;
    }
    runtime.process(mono.data(), rendered.data(), rendered.size());

    WavData output;
    output.sampleRate = input.sampleRate;
    output.channels = 1;
    output.samples = rendered;
    writeWavFile(outputPath, output);
    std::cout << "wrote " << outputPath << '\n';
    printStats(rendered);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
