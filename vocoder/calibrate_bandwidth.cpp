#include "host_plugin.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

static void makeNoiseBuffers(int frames, std::vector<float> &carrierL,
                             std::vector<float> &carrierR,
                             std::vector<float> &modL,
                             std::vector<float> &modR) {
  carrierL.resize(frames);
  carrierR.resize(frames);
  modL.resize(frames);
  modR.resize(frames);

  uint32_t state = 1u;
  for (int i = 0; i < frames; ++i) {
    state = state * 1664525u + 1013904223u;
    const float a = ((state >> 8) / 8388608.0f) * 2.0f - 1.0f;
    state = state * 1664525u + 1013904223u;
    const float b = ((state >> 8) / 8388608.0f) * 2.0f - 1.0f;
    carrierL[i] = a * 0.4f;
    carrierR[i] = b * 0.4f;
    modL[i] = a * 0.4f;
    modR[i] = b * 0.4f;
  }
}

static double measureRms(int bandCount, int bandwidth) {
  HostAlgorithm host = makeHostAlgorithm();
  hostSetParameter(host, kBandCount, (int16_t)bandCount);
  hostSetParameter(host, kBandWidth, (int16_t)bandwidth);
  hostSetParameter(host, kDepth, 70);
  hostSetParameter(host, kFormant, 0);
  hostSetParameter(host, kEnhance, 0);
  hostSetParameter(host, kWet, 100);

  const int frames = 48000 * 4;
  std::vector<float> carrierL, carrierR, modL, modR, outL, outR;
  makeNoiseBuffers(frames, carrierL, carrierR, modL, modR);
  renderHostAlgorithm(host, carrierL.data(), carrierR.data(), modL.data(),
                      modR.data(), frames, outL, outR);

  const int skip = 48000;
  double sumSq = 0.0;
  int count = 0;
  for (int i = skip; i < frames; ++i) {
    sumSq += outL[i] * outL[i] + outR[i] * outR[i];
    count += 2;
  }
  return sqrt(sumSq / (double)count);
}

int main() {
  std::filesystem::create_directories("vocoder/fixtures/analysis");
  std::ofstream csv("vocoder/fixtures/analysis/bandwidth_calibration.csv");
  if (!csv) {
    throw std::runtime_error("failed to create calibration csv");
  }

  const int widths[] = {0, 25, 50, 75, 100};
  const int bands[] = {4, 8, 16, 24, 32, 40};
  const double reference = measureRms(8, 0);

  csv << "bands,width,rms,inverse_relative\n";
  std::cout << "Bandwidth calibration estimates\n";
  for (int bandCount : bands) {
    for (int width : widths) {
      const double rms = measureRms(bandCount, width);
      const double inv = reference > 0.0 ? reference / rms : 0.0;
      csv << bandCount << "," << width << "," << std::fixed
          << std::setprecision(8) << rms << "," << inv << "\n";
      std::cout << "bands=" << bandCount << " width=" << width
                << " rms=" << rms << " inv=" << inv << "\n";
    }
  }

  return 0;
}
