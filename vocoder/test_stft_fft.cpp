#include "stft_fft.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "TEST FAILED: " << message << "\n";
    std::exit(1);
  }
}

static void testFftRoundTrip() {
  StftComplex data[kStftMaxFftSize];
  const int size = 128;
  for (int i = 0; i < size; ++i) {
    data[i].re = 0.4f * sinf(2.0f * 3.14159265359f * 7.0f * i / (float)size) +
                 0.2f * cosf(2.0f * 3.14159265359f * 11.0f * i / (float)size);
    data[i].im = 0.0f;
  }

  StftComplex original[kStftMaxFftSize];
  memcpy(original, data, sizeof(StftComplex) * size);

  stftFft(data, size, false);
  stftFft(data, size, true);

  for (int i = 0; i < size; ++i) {
    require(fabsf(data[i].re - original[i].re) < 1.0e-4f,
            "FFT round-trip real mismatch");
    require(fabsf(data[i].im - original[i].im) < 1.0e-4f,
            "FFT round-trip imag mismatch");
  }
}

static void testBandMapOrdering() {
  StftBandMap bands[kVocoderMaxBands];
  stftBuildLogBandMap(bands, 40, 256, 48000.0f, 35.0f, 18000.0f);
  for (int i = 0; i < 40; ++i) {
    require(bands[i].binLo >= 1, "band map lower bin valid");
    require(bands[i].binHi > bands[i].binLo, "band map bin width positive");
    if (i > 0) {
      require(bands[i].binLo >= bands[i - 1].binLo, "band map monotonic");
    }
  }
}

int main() {
  testFftRoundTrip();
  testBandMapOrdering();
  std::cout << "stft fft tests passed\n";
  return 0;
}
