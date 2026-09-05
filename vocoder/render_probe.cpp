// Offline reference renderer for the current native vocoder, without any
// peak normalization or additional limiting. Bus-voltage conversion is explicit.
#include "host_plugin.h"
#include "wav_io.h"
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;

struct Options {
  std::string input, output, metadata;
  int bands = 40, width = 100, depth = 100, formant = 0;
  int minHz = 30, maxHz = 18000, attackMs = 10, releaseMs = 30;
  int wet = 100, pregain = 0, enhance = 1;
  double settleSeconds = 0.5, tailSeconds = 0.5;
  double busVoltsPerFullScale = 5.0;
};

static uint16_t little16(const uint8_t *p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

static uint32_t little32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static void write16(std::ostream &stream, uint16_t value) {
  const char bytes[2] = {char(value), char(value >> 8)};
  stream.write(bytes, 2);
}

static void write32(std::ostream &stream, uint32_t value) {
  const char bytes[4] = {char(value), char(value >> 8), char(value >> 16),
                         char(value >> 24)};
  stream.write(bytes, 4);
}

static WavData readProbeWav(const std::string &path) {
  // wav_io.h's existing reader only handles PCM16. The Live probe generator
  // writes PCM24, and recorded/reused probes may contain IEEE floats.
  std::ifstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot open input WAV: " + path);
  stream.seekg(0, std::ios::end);
  const auto length = stream.tellg();
  stream.seekg(0);
  if (length < 12) throw std::runtime_error("input WAV is too short");
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  stream.read(reinterpret_cast<char *>(bytes.data()), length);
  if (!stream || memcmp(bytes.data(), "RIFF", 4) ||
      memcmp(bytes.data() + 8, "WAVE", 4))
    throw std::runtime_error("input must be a little-endian RIFF/WAVE file");
  if (uint64_t(little32(bytes.data() + 4)) + 8 > bytes.size())
    throw std::runtime_error("truncated RIFF file");
  uint16_t format = 0, channels = 0, bits = 0, alignment = 0;
  uint32_t sampleRate = 0;
  const uint8_t *audio = nullptr;
  size_t audioBytes = 0;
  bool haveFormat = false;
  for (size_t position = 12; position + 8 <= bytes.size();) {
    const auto *chunk = bytes.data() + position;
    const uint32_t size = little32(chunk + 4);
    position += 8;
    if (size > bytes.size() - position)
      throw std::runtime_error("truncated WAV chunk");
    const auto *data = bytes.data() + position;
    if (!memcmp(chunk, "fmt ", 4)) {
      if (haveFormat || size < 16)
        throw std::runtime_error("invalid or duplicate WAV format chunk");
      haveFormat = true;
      format = little16(data);
      channels = little16(data + 2);
      sampleRate = little32(data + 4);
      alignment = little16(data + 12);
      bits = little16(data + 14);
      if (format == 0xfffe) {
        if (size < 40) throw std::runtime_error("short extensible WAV format");
        format = little16(data + 24);
        const uint16_t validBits = little16(data + 18);
        if (validBits && validBits != bits)
          throw std::runtime_error("packed valid-bit WAV format is unsupported");
      }
    } else if (!memcmp(chunk, "data", 4)) {
      if (audio) throw std::runtime_error("multiple WAV data chunks are unsupported");
      audio = data;
      audioBytes = size;
    }
    position += size + (size & 1u);
  }
  if (!haveFormat || !audio || !audioBytes)
    throw std::runtime_error("input WAV is missing format or nonempty audio");
  if (sampleRate != NT_globals.sampleRate)
    throw std::runtime_error("host DSP requires a 48000 Hz input WAV; no resampling is performed");
  if (channels != 1 && channels != 2)
    throw std::runtime_error("input WAV must be mono or stereo");
  if ((format != 1 || (bits != 8 && bits != 16 && bits != 24 && bits != 32)) &&
      (format != 3 || (bits != 32 && bits != 64)))
    throw std::runtime_error("supported WAV samples: PCM8/16/24/32 or float32/64");
  const unsigned sampleBytes = bits / 8;
  if (alignment != channels * sampleBytes || audioBytes % alignment)
    throw std::runtime_error("invalid WAV frame alignment");
  WavData result;
  result.sampleRate = sampleRate;
  result.channels = channels;
  result.samples.resize(audioBytes / sampleBytes);
  for (size_t index = 0; index < result.samples.size(); ++index) {
    const auto *p = audio + index * sampleBytes;
    double value;
    if (format == 3 && bits == 32) {
      uint32_t raw = little32(p);
      float decoded;
      memcpy(&decoded, &raw, sizeof(decoded));
      value = decoded;
    } else if (format == 3) {
      uint64_t raw = uint64_t(little32(p)) | (uint64_t(little32(p + 4)) << 32);
      double decoded;
      memcpy(&decoded, &raw, sizeof(decoded));
      value = decoded;
    } else if (bits == 8) {
      value = (int(p[0]) - 128) / 128.0;
    } else {
      uint32_t raw = 0;
      for (unsigned byte = 0; byte < sampleBytes; ++byte)
        raw |= uint32_t(p[byte]) << (8 * byte);
      const int64_t signedValue = (raw & (uint32_t(1) << (bits - 1)))
                                     ? int64_t(raw) - (int64_t(1) << bits)
                                     : int64_t(raw);
      value = double(signedValue) / double(uint64_t(1) << (bits - 1));
    }
    if (!std::isfinite(value) || std::fabs(value) > std::numeric_limits<float>::max())
      throw std::runtime_error("input WAV contains non-finite or unrepresentable samples");
    result.samples[index] = static_cast<float>(value);
  }
  return result;
}

static void writeFloatWav(const std::string &path, const std::vector<float> &left,
                          const std::vector<float> &right, size_t frames) {
  if (frames > (std::numeric_limits<uint32_t>::max() - 48u) / 8u)
    throw std::runtime_error("output exceeds RIFF/WAVE's 4 GB limit");
  std::ofstream stream(path, std::ios::binary);
  if (!stream) throw std::runtime_error("cannot create output WAV: " + path);
  const uint32_t bytes = static_cast<uint32_t>(frames * 8);
  stream.write("RIFF", 4); write32(stream, 48 + bytes); stream.write("WAVE", 4);
  stream.write("fmt ", 4); write32(stream, 16); write16(stream, 3);
  write16(stream, 2); write32(stream, NT_globals.sampleRate);
  write32(stream, NT_globals.sampleRate * 8); write16(stream, 8); write16(stream, 32);
  stream.write("fact", 4); write32(stream, 4); write32(stream, static_cast<uint32_t>(frames));
  stream.write("data", 4); write32(stream, bytes);
  for (size_t frame = 0; frame < frames; ++frame) {
    for (float value : {left[frame], right[frame]}) {
      uint32_t raw;
      memcpy(&raw, &value, sizeof(raw));
      write32(stream, raw);
    }
  }
  stream.close();
  if (!stream) throw std::runtime_error("failed while writing output WAV");
}

static double number(const std::string &text, const std::string &name) {
  size_t used = 0;
  const double value = std::stod(text, &used);
  if (used != text.size() || !std::isfinite(value))
    throw std::runtime_error(name + " must be a finite number");
  return value;
}

static int parameter(const std::string &text, const std::string &name,
                     int index, double scale = 1) {
  const double value = number(text, name), raw = value * scale;
  const auto &descriptor = parameters[index];
  if (raw < descriptor.min || raw > descriptor.max)
    throw std::runtime_error(name + " outside native range " +
                             std::to_string(descriptor.min / scale) + ".." +
                             std::to_string(descriptor.max / scale));
  if (std::fabs(raw - std::round(raw)) > 1e-6)
    throw std::runtime_error(name + " requires increments of " + std::to_string(1 / scale));
  return static_cast<int>(std::round(raw));
}

static double duration(const std::string &text, const std::string &name) {
  const double value = number(text, name);
  if (value < 0 || value > 120)
    throw std::runtime_error(name + " must be between 0 and 120 seconds");
  return value;
}

static void help() {
  std::cout << "Usage: render_probe --input IN.wav --output OUT.wav [options]\n"
      "Self-modulated current native vocoder; input must be mono/stereo 48000 Hz.\n"
      "Output is stereo float32 WAV, including leading settling silence and tail.\n"
      "WAV +/-1 maps to +/-5V on the DSP buses by default; output divides by the same scale.\n"
      "Native DSP guards remain active; no peak normalization or additional limiting.\n\n"
      "  --bands N                 4..40, default 40\n"
      "  --width PERCENT           0..200, default 100\n"
      "  --enhance N               0=off, 1=on (default)\n"
      "  --depth PERCENT           0..200, default 100\n"
      "  --formant-semitones N     -36..36 in 0.1 steps, default 0\n"
      "  --min-hz N                20..1000, default 30\n"
      "  --max-hz N                2000..20000, default 18000\n"
      "  --attack-ms N             1..500, default 10\n"
      "  --release-ms N            1..1000, default 30\n"
      "  --wet PERCENT             0..100, default 100\n"
      "  --pregain-db N            -60..12 in 0.1 steps, default 0\n"
      "  --settle-seconds N        0..120, default 0.5\n"
      "  --tail-seconds N          0..120, default 0.5\n"
      "  --bus-volts-per-full-scale N  0.01..100, default 5; use 1 for raw bus units\n"
      "  --metadata FILE.json      Also save the settings/statistics JSON printed to stdout\n"
      "All unscaled controls require integer values. Unsupported values are rejected.\n"
      "The native Enhance parameter is reserved and has no DSP implementation.\n";
}

static std::string quoted(const std::string &text) {
  std::ostringstream out;
  out << '"';
  for (unsigned char character : text) {
    if (character == '"' || character == '\\') out << '\\' << character;
    else if (character < 0x20)
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(character) << std::dec;
    else out << character;
  }
  out << '"';
  return out.str();
}

int main(int argc, char **argv) {
  try {
    Options options;
    for (int arg = 1; arg < argc; ++arg) {
      const std::string name = argv[arg];
      if (name == "--help" || name == "-h") { help(); return 0; }
      if (arg + 1 >= argc) throw std::runtime_error("missing value for " + name);
      const std::string value = argv[++arg];
      if (name == "--input") options.input = value;
      else if (name == "--output") options.output = value;
      else if (name == "--metadata") options.metadata = value;
      else if (name == "--bands") options.bands = parameter(value, name, kBandCount);
      else if (name == "--width") options.width = parameter(value, name, kBandWidth);
      else if (name == "--enhance") options.enhance = parameter(value, name, kEnhance);
      else if (name == "--depth") options.depth = parameter(value, name, kDepth);
      else if (name == "--formant-semitones") options.formant = parameter(value, name, kFormant, 10);
      else if (name == "--min-hz") options.minHz = parameter(value, name, kMinFreq);
      else if (name == "--max-hz") options.maxHz = parameter(value, name, kMaxFreq);
      else if (name == "--attack-ms") options.attackMs = parameter(value, name, kAttack);
      else if (name == "--release-ms") options.releaseMs = parameter(value, name, kRelease);
      else if (name == "--wet") options.wet = parameter(value, name, kWet);
      else if (name == "--pregain-db") options.pregain = parameter(value, name, kPreGain, 10);
      else if (name == "--settle-seconds") options.settleSeconds = duration(value, name);
      else if (name == "--tail-seconds") options.tailSeconds = duration(value, name);
      else if (name == "--bus-volts-per-full-scale") {
        options.busVoltsPerFullScale = number(value, name);
        if (options.busVoltsPerFullScale < .01 || options.busVoltsPerFullScale > 100)
          throw std::runtime_error(name + " must be between 0.01 and 100");
      }
      else throw std::runtime_error("unknown option: " + name);
    }
    if (options.input.empty() || options.output.empty())
      throw std::runtime_error("--input and --output are required (see --help)");
    const fs::path inputPath = fs::weakly_canonical(options.input);
    const fs::path outputPath = fs::weakly_canonical(options.output);
    if (inputPath == outputPath)
      throw std::runtime_error("output must be different from the input WAV");
    if (!options.metadata.empty() &&
        (fs::weakly_canonical(options.metadata) == inputPath ||
         fs::weakly_canonical(options.metadata) == outputPath))
      throw std::runtime_error("metadata must be separate from input/output WAVs");
    const WavData input = readProbeWav(options.input);
    const size_t inputFrames = input.samples.size() / input.channels;
    const size_t settleFrames = std::llround(options.settleSeconds * NT_globals.sampleRate);
    const size_t tailFrames = std::llround(options.tailSeconds * NT_globals.sampleRate);
    const size_t frames = settleFrames + inputFrames + tailFrames;
    if (frames > static_cast<size_t>(std::numeric_limits<int>::max() - 23))
      throw std::runtime_error("audio exceeds the host render length limit");
    const size_t paddedFrames = ((frames + 23) / 24) * 24;
    std::vector<float> left(paddedFrames, 0), right(paddedFrames, 0);
    for (size_t frame = 0; frame < inputFrames; ++frame) {
      left[settleFrames + frame] = input.samples[frame * input.channels] * options.busVoltsPerFullScale;
      right[settleFrames + frame] = input.samples[frame * input.channels + (input.channels == 2 ? 1 : 0)] * options.busVoltsPerFullScale;
    }
    HostAlgorithm host = makeHostAlgorithm();
    // The helper returns a named object; rebind inline arrays even if a host
    // compiler does not perform named return-value optimization.
    host.algorithm->v = host.values;
    host.algorithm->vIncludingCommon = host.commonValues;
    const std::pair<int, int> settings[] = {
      {kInCarrier, 1}, {kCarrierStereo, input.channels == 2},
      {kInModulator, 3}, {kModulatorStereo, input.channels == 2},
      {kOut, 13}, {kOutMode, 1}, {kBandCount, options.bands},
      {kBandWidth, options.width}, {kDepth, options.depth}, {kFormant, options.formant},
      {kMinFreq, options.minHz}, {kMaxFreq, options.maxHz}, {kAttack, options.attackMs},
      {kRelease, options.releaseMs}, {kEnhance, options.enhance}, {kWet, options.wet}, {kPreGain, options.pregain},
    };
    for (const auto &setting : settings)
      hostSetParameter(host, setting.first, static_cast<int16_t>(setting.second));
    std::vector<float> outputLeft, outputRight;
    renderHostAlgorithm(host, left.data(), right.data(), left.data(), right.data(),
                        static_cast<int>(paddedFrames), outputLeft, outputRight);
    double squares[2] = {}, peaks[2] = {};
    for (size_t frame = 0; frame < frames; ++frame) {
      outputLeft[frame] /= options.busVoltsPerFullScale;
      outputRight[frame] /= options.busVoltsPerFullScale;
      const float samples[2] = {outputLeft[frame], outputRight[frame]};
      for (int channel = 0; channel < 2; ++channel) {
        const double value = samples[channel];
        if (!std::isfinite(value)) throw std::runtime_error("native DSP produced non-finite audio");
        peaks[channel] = std::max(peaks[channel], std::fabs(value));
        squares[channel] += value * value;
      }
    }
    writeFloatWav(options.output, outputLeft, outputRight, frames);
    const auto *algorithm = static_cast<const _vocoderAlgorithm *>(host.algorithm);
    const auto &descriptor = *algorithm->descriptor;
    std::ostringstream report;
    report << std::setprecision(10)
      << "{\n  \"input\": " << quoted(inputPath.string())
      << ",\n  \"output\": " << quoted(outputPath.string())
      << ",\n  \"mode\": \"self_modulated\",\n  \"sample_rate\": 48000,\n  \"input_channels\": " << input.channels
      << ",\n  \"output_channels\": 2,\n  \"output_encoding\": \"float32\",\n  \"input_frames\": " << inputFrames
      << ",\n  \"input_start_frame\": " << settleFrames << ",\n  \"tail_frames\": " << tailFrames
      << ",\n  \"output_frames\": " << frames << ",\n  \"internal_padding_frames\": " << paddedFrames - frames
      << ",\n  \"bus_volts_per_full_scale\": " << options.busVoltsPerFullScale
      << ",\n  \"applied_gain_after_dsp\": " << 1.0 / options.busVoltsPerFullScale
      << ",\n  \"filter_stages_per_band\": " << kVocoderFilterStages
      << ",\n  \"native_enhance_implemented\": true,\n  \"parameters\": {"
      << "\"bands\":" << options.bands << ",\"width_percent\":" << options.width
      << ",\"enhance\":" << options.enhance
      << ",\"depth_percent\":" << options.depth << ",\"formant_semitones\":" << options.formant / 10.0
      << ",\"min_hz\":" << options.minHz << ",\"max_hz\":" << options.maxHz
      << ",\"attack_ms\":" << options.attackMs << ",\"release_ms\":" << options.releaseMs
      << ",\"wet_percent\":" << options.wet << ",\"pregain_db\":" << options.pregain / 10.0 << "},\n"
      << "  \"final_effective_controls\": {\"bands\":" << descriptor.activeBands
      << ",\"width_percent\":" << algorithm->controls.currentBandwidth
      << ",\"formant_semitones\":" << algorithm->controls.currentFormant / 10.0
      << ",\"wet_percent\":" << algorithm->controls.currentWet
      << ",\"pregain_db\":" << algorithm->controls.currentOutputGainDb
      << ",\"analysis_min_hz\":" << descriptor.analysisFreq[0]
      << ",\"analysis_max_hz\":" << descriptor.analysisFreq[descriptor.activeBands - 1]
      << ",\"analysis_q\":" << descriptor.analysisQ << ",\"synthesis_q\":" << descriptor.synthesisQ << "},\n"
      << "  \"peak\": [" << peaks[0] << ',' << peaks[1] << "],\n  \"rms\": ["
      << std::sqrt(squares[0] / frames) << ',' << std::sqrt(squares[1] / frames) << "]\n}\n";
    if (!options.metadata.empty()) {
      std::ofstream metadata(options.metadata);
      metadata << report.str();
      metadata.close();
      if (!metadata) throw std::runtime_error("failed to write metadata JSON");
    }
    std::cout << report.str();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "render_probe: " << error.what() << '\n';
    return 1;
  }
}
