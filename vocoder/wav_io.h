#ifndef VOCODER_WAV_IO_H
#define VOCODER_WAV_IO_H

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

struct WavData {
  int sampleRate = 48000;
  int channels = 2;
  std::vector<float> samples;
};

inline uint32_t readU32(std::ifstream &stream) {
  uint32_t value = 0;
  stream.read(reinterpret_cast<char *>(&value), sizeof(value));
  return value;
}

inline uint16_t readU16(std::ifstream &stream) {
  uint16_t value = 0;
  stream.read(reinterpret_cast<char *>(&value), sizeof(value));
  return value;
}

inline WavData readWavFile(const std::string &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to open wav: " + path);
  }

  char riff[4] = {};
  char wave[4] = {};
  stream.read(riff, 4);
  (void)readU32(stream);
  stream.read(wave, 4);
  if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE") {
    throw std::runtime_error("invalid wav header: " + path);
  }

  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  std::vector<int16_t> pcm;

  while (stream) {
    char chunkId[4] = {};
    stream.read(chunkId, 4);
    if (!stream) {
      break;
    }
    const uint32_t chunkSize = readU32(stream);
    const std::string id(chunkId, 4);

    if (id == "fmt ") {
      const uint16_t format = readU16(stream);
      channels = readU16(stream);
      sampleRate = readU32(stream);
      (void)readU32(stream);
      (void)readU16(stream);
      bitsPerSample = readU16(stream);
      if (chunkSize > 16) {
        stream.seekg(chunkSize - 16, std::ios::cur);
      }
      if (format != 1 || bitsPerSample != 16) {
        throw std::runtime_error("only 16-bit PCM wav supported: " + path);
      }
    } else if (id == "data") {
      pcm.resize(chunkSize / sizeof(int16_t));
      stream.read(reinterpret_cast<char *>(pcm.data()), chunkSize);
    } else {
      stream.seekg(chunkSize, std::ios::cur);
    }
  }

  if (channels == 0 || sampleRate == 0 || pcm.empty()) {
    throw std::runtime_error("missing wav chunks: " + path);
  }

  WavData wav;
  wav.sampleRate = (int)sampleRate;
  wav.channels = (int)channels;
  wav.samples.resize(pcm.size());
  for (size_t i = 0; i < pcm.size(); ++i) {
    wav.samples[i] = pcm[i] / 32768.0f;
  }
  return wav;
}

inline void writeWavFile(const std::string &path, const WavData &wav) {
  std::ofstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("failed to create wav: " + path);
  }

  std::vector<int16_t> pcm(wav.samples.size());
  for (size_t i = 0; i < wav.samples.size(); ++i) {
    const float clamped = std::max(-1.0f, std::min(1.0f, wav.samples[i]));
    pcm[i] = (int16_t)(clamped * 32767.0f);
  }

  const uint32_t dataSize = (uint32_t)(pcm.size() * sizeof(int16_t));
  const uint32_t fileSize = 36 + dataSize;
  const uint16_t channels = (uint16_t)wav.channels;
  const uint32_t sampleRate = (uint32_t)wav.sampleRate;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
  const uint16_t blockAlign = channels * bitsPerSample / 8;
  const uint32_t fmtSize = 16;
  const uint16_t format = 1;

  stream.write("RIFF", 4);
  stream.write(reinterpret_cast<const char *>(&fileSize), sizeof(fileSize));
  stream.write("WAVE", 4);
  stream.write("fmt ", 4);
  stream.write(reinterpret_cast<const char *>(&fmtSize), sizeof(fmtSize));
  stream.write(reinterpret_cast<const char *>(&format), sizeof(format));
  stream.write(reinterpret_cast<const char *>(&channels), sizeof(channels));
  stream.write(reinterpret_cast<const char *>(&sampleRate), sizeof(sampleRate));
  stream.write(reinterpret_cast<const char *>(&byteRate), sizeof(byteRate));
  stream.write(reinterpret_cast<const char *>(&blockAlign), sizeof(blockAlign));
  stream.write(reinterpret_cast<const char *>(&bitsPerSample),
               sizeof(bitsPerSample));
  stream.write("data", 4);
  stream.write(reinterpret_cast<const char *>(&dataSize), sizeof(dataSize));
  stream.write(reinterpret_cast<const char *>(pcm.data()), dataSize);
}

#endif // VOCODER_WAV_IO_H
