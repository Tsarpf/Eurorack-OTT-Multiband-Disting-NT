#include "wav_io.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static float softClip(float x) { return tanhf(x); }

static float saw(float phase) { return 2.0f * phase - 1.0f; }

static float fract(float x) { return x - floorf(x); }

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

static void fixtureImpulse(const fs::path &dir, int sr, int frames) {
  std::vector<float> carrierL(frames), carrierR(frames), modL(frames, 0.0f),
      modR(frames, 0.0f);
  modL[0] = 1.0f;
  modR[0] = 1.0f;
  for (int i = 0; i < frames; ++i) {
    const float p = fract(110.0f * i / (float)sr);
    const float s = saw(p);
    carrierL[i] = 0.7f * s;
    carrierR[i] = 0.7f * s;
  }
  writeStereo(dir / "01_impulse_carrier.wav", sr, carrierL, carrierR);
  writeStereo(dir / "01_impulse_modulator.wav", sr, modL, modR);
}

static uint32_t lcg(uint32_t &state) {
  state = state * 1664525u + 1013904223u;
  return state;
}

static float whiteNoise(uint32_t &state) {
  return ((lcg(state) >> 8) / 8388608.0f) * 2.0f - 1.0f;
}

static void fixtureNoise(const fs::path &dir, int sr, int frames) {
  std::vector<float> carrierL(frames), carrierR(frames), modL(frames),
      modR(frames);
  uint32_t state = 1u;
  for (int i = 0; i < frames; ++i) {
    const float p = fract(82.41f * i / (float)sr);
    const float q = fract(164.82f * i / (float)sr);
    carrierL[i] = 0.5f * saw(p) + 0.2f * sinf(2.0f * 3.14159265359f * q);
    carrierR[i] = carrierL[i];
    const float env = 0.8f + 0.2f * sinf(2.0f * 3.14159265359f * 0.7f * i / sr);
    modL[i] = env * whiteNoise(state);
    modR[i] = modL[i];
  }
  writeStereo(dir / "02_noise_carrier.wav", sr, carrierL, carrierR);
  writeStereo(dir / "02_noise_modulator.wav", sr, modL, modR);
}

static void fixtureSine(const fs::path &dir, int sr, int frames) {
  std::vector<float> carrierL(frames), carrierR(frames), modL(frames),
      modR(frames);
  for (int i = 0; i < frames; ++i) {
    const float t = i / (float)sr;
    const float a = saw(fract(98.0f * t));
    const float b = saw(fract(196.0f * t));
    carrierL[i] = 0.5f * a + 0.25f * b;
    carrierR[i] = carrierL[i];
    modL[i] = 0.85f * sinf(2.0f * 3.14159265359f * 1000.0f * t) *
              (0.6f + 0.4f * sinf(2.0f * 3.14159265359f * 2.0f * t));
    modR[i] = modL[i];
  }
  writeStereo(dir / "03_sine_carrier.wav", sr, carrierL, carrierR);
  writeStereo(dir / "03_sine_modulator.wav", sr, modL, modR);
}

static void fixtureMotionSweep(const fs::path &dir, int sr, int frames) {
  std::vector<float> carrierL(frames), carrierR(frames), modL(frames),
      modR(frames);
  uint32_t state = 2u;
  for (int i = 0; i < frames; ++i) {
    const float t = i / (float)sr;
    carrierL[i] = 0.45f * saw(fract(110.0f * t)) +
                  0.2f * saw(fract(111.4f * t));
    carrierR[i] = 0.45f * saw(fract(109.2f * t)) +
                  0.2f * saw(fract(110.6f * t));

    const float sweepFreq = 120.0f * powf(50.0f, t / 5.0f);
    const float chirp = sinf(2.0f * 3.14159265359f * sweepFreq * t);
    const float noise = 0.2f * whiteNoise(state);
    modL[i] = 0.7f * chirp + noise;
    modR[i] = modL[i];
  }
  writeStereo(dir / "04_motion_sweep_carrier.wav", sr, carrierL, carrierR);
  writeStereo(dir / "04_motion_sweep_modulator.wav", sr, modL, modR);
}

static void fixtureNeuroBass(const fs::path &dir, int sr, int frames) {
  std::vector<float> carrierL(frames), carrierR(frames), modL(frames),
      modR(frames);
  uint32_t state = 3u;
  for (int i = 0; i < frames; ++i) {
    const float t = i / (float)sr;
    const float wobble = 0.5f + 0.5f * sinf(2.0f * 3.14159265359f * 1.5f * t);
    const float bass = 0.55f * sinf(2.0f * 3.14159265359f * 55.0f * t);
    const float growl = 0.45f * saw(fract((110.0f + 20.0f * wobble) * t));
    carrierL[i] = 0.75f * softClip(bass + growl);
    carrierR[i] = carrierL[i];

    const float syllable = 0.5f + 0.5f * sinf(2.0f * 3.14159265359f * 4.0f * t);
    const float formant1 = sinf(2.0f * 3.14159265359f * 400.0f * t);
    const float formant2 = sinf(2.0f * 3.14159265359f * 1800.0f * t);
    const float breath = 0.25f * whiteNoise(state);
    modL[i] = 0.55f * syllable * (0.7f * formant1 + 0.3f * formant2) + breath;
    modR[i] = modL[i];
  }
  writeStereo(dir / "05_neuro_bass_carrier.wav", sr, carrierL, carrierR);
  writeStereo(dir / "05_neuro_bass_modulator.wav", sr, modL, modR);
}

int main() {
  const int sampleRate = 48000;
  const int seconds = 5;
  const int frames = sampleRate * seconds;
  const fs::path inputDir = "vocoder/fixtures/input";
  fs::create_directories(inputDir);

  fixtureImpulse(inputDir, sampleRate, frames);
  fixtureNoise(inputDir, sampleRate, frames);
  fixtureSine(inputDir, sampleRate, frames);
  fixtureMotionSweep(inputDir, sampleRate, frames);
  fixtureNeuroBass(inputDir, sampleRate, frames);

  std::cout << "Generated fixtures in " << inputDir << "\n";
  return 0;
}
