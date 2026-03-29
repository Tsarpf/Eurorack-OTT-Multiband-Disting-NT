#ifndef VOCODER_STFT_FFT_H
#define VOCODER_STFT_FFT_H

#include "vocoder_dsp.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>

static const int kStftMaxFftSize = 256;
static const int kStftMaxBins = (kStftMaxFftSize / 2) + 1;

struct StftComplex {
  float re;
  float im;
};

inline void stftSwapComplex(StftComplex &a, StftComplex &b) {
  const StftComplex tmp = a;
  a = b;
  b = tmp;
}

inline unsigned stftReverseBits(unsigned value, int bits) {
  unsigned reversed = 0;
  for (int i = 0; i < bits; ++i) {
    reversed = (reversed << 1) | (value & 1u);
    value >>= 1u;
  }
  return reversed;
}

inline void stftFft(StftComplex *data, int size, bool inverse) {
  int log2Size = 0;
  for (int n = size; n > 1; n >>= 1) {
    ++log2Size;
  }

  for (int i = 0; i < size; ++i) {
    const unsigned j = stftReverseBits((unsigned)i, log2Size);
    if ((unsigned)i < j) {
      stftSwapComplex(data[i], data[j]);
    }
  }

  for (int len = 2; len <= size; len <<= 1) {
    const float angleSign = inverse ? 1.0f : -1.0f;
    const float angleStep = angleSign * 2.0f * 3.14159265359f / (float)len;
    for (int start = 0; start < size; start += len) {
      for (int i = 0; i < (len >> 1); ++i) {
        const float angle = angleStep * (float)i;
        const float wr = cosf(angle);
        const float wi = sinf(angle);
        const StftComplex even = data[start + i];
        const StftComplex odd = data[start + i + (len >> 1)];
        const StftComplex twiddled = {
            wr * odd.re - wi * odd.im,
            wr * odd.im + wi * odd.re,
        };
        data[start + i].re = even.re + twiddled.re;
        data[start + i].im = even.im + twiddled.im;
        data[start + i + (len >> 1)].re = even.re - twiddled.re;
        data[start + i + (len >> 1)].im = even.im - twiddled.im;
      }
    }
  }

  if (inverse) {
    const float scale = 1.0f / (float)size;
    for (int i = 0; i < size; ++i) {
      data[i].re *= scale;
      data[i].im *= scale;
    }
  }
}

inline void stftMakeHannWindow(float *window, int size) {
  if (size <= 1) {
    if (size == 1) {
      window[0] = 1.0f;
    }
    return;
  }
  for (int i = 0; i < size; ++i) {
    window[i] =
        0.5f - 0.5f * cosf((2.0f * 3.14159265359f * (float)i) / (float)(size - 1));
  }
}

inline void stftApplyWindow(const float *input, const float *window,
                            StftComplex *spectrum, int size) {
  for (int i = 0; i < size; ++i) {
    spectrum[i].re = input[i] * window[i];
    spectrum[i].im = 0.0f;
  }
}

struct StftBandMap {
  int binLo;
  int binHi;
};

inline void stftBuildLogBandMap(StftBandMap *bands, int bandCount, int fftSize,
                                float sampleRate, float minFreq,
                                float maxFreq) {
  if (bandCount <= 0) {
    return;
  }
  const float clampedMin = vocoderClamp(minFreq, 20.0f, sampleRate * 0.45f);
  const float clampedMax = vocoderClamp(
      maxFreq > clampedMin ? maxFreq : clampedMin + 20.0f, clampedMin + 1.0f,
      sampleRate * 0.49f);
  const float step = bandCount > 1
                         ? powf(clampedMax / clampedMin, 1.0f / (float)(bandCount - 1))
                         : 1.0f;
  const float binScale = (float)fftSize / sampleRate;

  for (int band = 0; band < bandCount; ++band) {
    const float centre = clampedMin * powf(step, (float)band);
    const float lower = band == 0 ? clampedMin : centre / sqrtf(step);
    const float upper = band == bandCount - 1 ? clampedMax : centre * sqrtf(step);
    int binLo = (int)(lower * binScale);
    int binHi = (int)(upper * binScale);
    if (binLo < 1) {
      binLo = 1;
    }
    if (binHi <= binLo) {
      binHi = binLo + 1;
    }
    if (binHi > (fftSize >> 1)) {
      binHi = fftSize >> 1;
    }
    bands[band].binLo = binLo;
    bands[band].binHi = binHi;
  }
}

#endif // VOCODER_STFT_FFT_H
