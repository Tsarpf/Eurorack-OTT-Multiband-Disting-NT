#include "host_plugin.h"
#include "wav_io.h"
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct FixtureSpec {
  std::string name;
  int bandCount;
  int width;
  int depth;
  int formant;
  int minFreq;
  int maxFreq;
  int attack;
  int release;
  int enhance;
  int wet;
  bool movingControls;
};

static void splitStereo(const WavData &wav, std::vector<float> &left,
                        std::vector<float> &right) {
  left.resize(wav.samples.size() / 2);
  right.resize(wav.samples.size() / 2);
  for (size_t i = 0; i < left.size(); ++i) {
    left[i] = wav.samples[i * 2 + 0];
    right[i] = wav.samples[i * 2 + 1];
  }
}

static void writeStereo(const fs::path &path, int sampleRate,
                        const std::vector<float> &left,
                        const std::vector<float> &right) {
  WavData wav;
  wav.sampleRate = sampleRate;
  wav.channels = 2;
  wav.samples.resize(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    wav.samples[i * 2 + 0] = left[i];
    wav.samples[i * 2 + 1] = right[i];
  }
  writeWavFile(path.string(), wav);
}

static void applySpec(HostAlgorithm &host, const FixtureSpec &spec) {
  hostSetParameter(host, kBandCount, (int16_t)spec.bandCount);
  hostSetParameter(host, kBandWidth, (int16_t)spec.width);
  hostSetParameter(host, kDepth, (int16_t)spec.depth);
  hostSetParameter(host, kFormant, (int16_t)spec.formant);
  hostSetParameter(host, kMinFreq, (int16_t)spec.minFreq);
  hostSetParameter(host, kMaxFreq, (int16_t)spec.maxFreq);
  hostSetParameter(host, kAttack, (int16_t)spec.attack);
  hostSetParameter(host, kRelease, (int16_t)spec.release);
  hostSetParameter(host, kEnhance, (int16_t)spec.enhance);
  hostSetParameter(host, kWet, (int16_t)spec.wet);
}

static void appendStats(std::ostream &stream, const FixtureSpec &spec,
                        const std::vector<float> &outL,
                        const std::vector<float> &outR, float rawPeak,
                        float appliedGain) {
  float peak = 0.0f;
  double sumSq = 0.0;
  for (size_t i = 0; i < outL.size(); ++i) {
    const float l = outL[i];
    const float r = outR[i];
    peak = std::max(peak, std::max(fabsf(l), fabsf(r)));
    sumSq += l * l + r * r;
  }
  const double rms = sqrt(sumSq / (double)(outL.size() * 2));

  stream << spec.name << ",bands=" << spec.bandCount << ",width=" << spec.width
         << ",depth=" << spec.depth << ",formant=" << spec.formant
         << ",enhance=" << spec.enhance << ",raw_peak=" << std::fixed
         << std::setprecision(6) << rawPeak << ",gain=" << appliedGain
         << ",peak=" << peak << ",rms=" << rms << "\n";
}

int main() {
  const fs::path inputDir = "vocoder/fixtures/input";
  const fs::path outputDir = "vocoder/fixtures/output";
  const fs::path analysisDir = "vocoder/fixtures/analysis";
  fs::create_directories(outputDir);
  fs::create_directories(analysisDir);

  const FixtureSpec specs[] = {
      {"01_impulse", 8, 35, 75, 0, 100, 10000, 5, 80, 1, 100, false},
      {"02_noise", 16, 50, 70, 0, 100, 10000, 10, 120, 1, 100, false},
      {"03_sine", 16, 40, 60, 12, 100, 10000, 12, 150, 0, 100, false},
      {"04_motion_sweep", 16, 50, 75, 0, 100, 10000, 8, 140, 1, 100, true},
      {"05_neuro_bass", 24, 45, 85, -24, 60, 8000, 7, 120, 1, 100, true},
  };

  std::ofstream report(analysisDir / "render_report.txt");
  if (!report) {
    throw std::runtime_error("failed to create render report");
  }

  for (const FixtureSpec &spec : specs) {
    const WavData carrier = readWavFile((inputDir / (spec.name + "_carrier.wav")).string());
    const WavData modulator =
        readWavFile((inputDir / (spec.name + "_modulator.wav")).string());

    std::vector<float> carL, carR, modL, modR;
    splitStereo(carrier, carL, carR);
    splitStereo(modulator, modL, modR);

    HostAlgorithm host = makeHostAlgorithm();
    applySpec(host, spec);

    std::vector<float> outL, outR;
    renderHostAlgorithm(
        host, carL.data(), carR.data(), modL.data(), modR.data(), (int)carL.size(),
        outL, outR,
        [&](HostAlgorithm &algorithm, int offset, int) {
          if (!spec.movingControls) {
            return;
          }
          const float t = offset / 48000.0f;
          const int formant =
              (int)(spec.formant + 72.0f * sinf(2.0f * 3.14159265359f * 0.45f * t));
          const int width =
              (int)(spec.width + 20.0f * sinf(2.0f * 3.14159265359f * 0.31f * t));
          hostSetParameter(algorithm, kFormant, (int16_t)formant);
          hostSetParameter(algorithm, kBandWidth,
                           (int16_t)(width < 0 ? 0 : (width > 100 ? 100 : width)));
        });

    float rawPeak = 0.0f;
    for (size_t i = 0; i < outL.size(); ++i) {
      rawPeak = std::max(rawPeak, std::max(fabsf(outL[i]), fabsf(outR[i])));
    }
    const float gain = rawPeak > 0.95f ? 0.95f / rawPeak : 1.0f;
    if (gain != 1.0f) {
      for (size_t i = 0; i < outL.size(); ++i) {
        outL[i] *= gain;
        outR[i] *= gain;
      }
    }

    writeStereo(outputDir / (spec.name + "_output.wav"), carrier.sampleRate, outL,
                outR);
    appendStats(report, spec, outL, outR, rawPeak, gain);
    std::cout << "Rendered " << spec.name << "\n";
  }

  return 0;
}
