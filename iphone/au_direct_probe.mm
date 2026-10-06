#import <AudioToolbox/AUAudioUnitImplementation.h>
#import <AVFAudio/AVAudioFormat.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The implementation lives in VocoderAU.mm (the same object that is linked
// into the extension).  This probe constructs that class directly so it can
// run as a jailbreak-side command-line tool even when SpringBoard keeps the
// app suspended on the lock screen.
@interface VocoderAudioUnit : AUAudioUnit
@end

static void printFormat(AUAudioUnit *unit) {
  AVAudioFormat *input = unit.inputBusses[0].format;
  AVAudioFormat *output = unit.outputBusses[0].format;
  const AudioStreamBasicDescription *inDesc = input.streamDescription;
  const AudioStreamBasicDescription *outDesc = output.streamDescription;
  std::printf("format: input %.0f Hz %u ch flags=0x%08x; output %.0f Hz %u ch flags=0x%08x\n",
              inDesc->mSampleRate, inDesc->mChannelsPerFrame,
              (unsigned)inDesc->mFormatFlags, outDesc->mSampleRate,
              outDesc->mChannelsPerFrame, (unsigned)outDesc->mFormatFlags);
}

static bool writeWav(const char *path, const std::vector<float> &left,
                     const std::vector<float> &right) {
  if (!path || left.size() != right.size()) return false;
  FILE *file = std::fopen(path, "wb");
  if (!file) return false;
  const uint32_t channels = 2;
  const uint32_t sampleRate = 48000;
  const uint32_t bits = 32;
  const uint32_t blockAlign = channels * sizeof(float);
  const uint32_t byteRate = sampleRate * blockAlign;
  const uint32_t dataSize = static_cast<uint32_t>(left.size() * blockAlign);
  const uint32_t riffSize = 36 + dataSize;
  auto put = [&](const void *data, size_t size) {
    return std::fwrite(data, 1, size, file) == size;
  };
  const uint16_t format = 3; // IEEE-754 float
  const uint16_t channelCount = static_cast<uint16_t>(channels);
  const uint16_t bitCount = static_cast<uint16_t>(bits);
  const uint32_t fmtSize = 16;
  bool ok = put("RIFF", 4) && put(&riffSize, 4) && put("WAVE", 4) &&
            put("fmt ", 4) && put(&fmtSize, 4) && put(&format, 2) &&
            put(&channelCount, 2) && put(&sampleRate, 4) && put(&byteRate, 4) &&
            put(&blockAlign, 2) && put(&bitCount, 2) && put("data", 4) &&
            put(&dataSize, 4);
  for (size_t i = 0; ok && i < left.size(); ++i) {
    ok = put(&left[i], sizeof(float)) && put(&right[i], sizeof(float));
  }
  std::fclose(file);
  return ok;
}

int main(int argc, char **argv) {
  @autoreleasepool {
    constexpr double kPi = 3.14159265358979323846;
    AudioComponentDescription desc = {kAudioUnitType_Effect, 'VCDR', 'TSPF', 0, 0};
    NSError *error = nil;
    VocoderAudioUnit *unit = [[VocoderAudioUnit alloc]
        initWithComponentDescription:desc options:0 error:&error];
    if (!unit) {
      std::fprintf(stderr, "direct init failed: %s\n",
                   error.localizedDescription.UTF8String ?: "unknown");
      return 2;
    }
    printFormat(unit);
    NSError *resourceError = nil;
    if (![unit allocateRenderResourcesAndReturnError:&resourceError]) {
      std::fprintf(stderr, "direct allocate failed: %s\n",
                   resourceError.localizedDescription.UTF8String ?: "unknown");
      return 3;
    }

    constexpr AUAudioFrameCount kFrames = 256;
    constexpr unsigned kBlocks = 32;
    std::vector<float> input0(kFrames), input1(kFrames);
    std::vector<float> output0(kFrames), output1(kFrames);
    std::vector<float> rendered0;
    std::vector<float> rendered1;
    rendered0.reserve(kFrames * kBlocks);
    rendered1.reserve(kFrames * kBlocks);
    AUInternalRenderBlock render = unit.internalRenderBlock;
    if (!render) {
      std::fprintf(stderr, "direct render block is nil\n");
      [unit deallocateRenderResources];
      return 4;
    }

    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    float outputPeak = 0.0f;
    unsigned renderedBlocks = 0;
    for (unsigned block = 0; block < kBlocks; ++block) {
      for (AUAudioFrameCount frame = 0; frame < kFrames; ++frame) {
        const double t = (double)(block * kFrames + frame) / 48000.0;
        input0[frame] = 0.55f * std::sin(2.0 * kPi * 220.0 * t);
        input1[frame] = 0.35f * std::sin(2.0 * kPi * 3.0 * t);
        inputEnergy += (double)input0[frame] * input0[frame];
        inputEnergy += (double)input1[frame] * input1[frame];
      }

      struct TwoBufferList {
        UInt32 mNumberBuffers;
        AudioBuffer mBuffers[2];
      } output = {};
      output.mNumberBuffers = 2;
      output.mBuffers[0].mNumberChannels = 1;
      output.mBuffers[0].mDataByteSize = kFrames * sizeof(float);
      output.mBuffers[0].mData = output0.data();
      output.mBuffers[1].mNumberChannels = 1;
      output.mBuffers[1].mDataByteSize = kFrames * sizeof(float);
      output.mBuffers[1].mData = output1.data();

      AURenderPullInputBlock pull = ^AUAudioUnitStatus(
          AudioUnitRenderActionFlags *actionFlags, const AudioTimeStamp *timestamp,
          AUAudioFrameCount frameCount, NSInteger inputBusNumber,
          AudioBufferList *input) {
        (void)actionFlags;
        (void)timestamp;
        (void)inputBusNumber;
        if (!input || frameCount > kFrames || input->mNumberBuffers < 2) {
          return kAudioUnitErr_InvalidPropertyValue;
        }
        std::memcpy(input->mBuffers[0].mData, input0.data(),
                    frameCount * sizeof(float));
        std::memcpy(input->mBuffers[1].mData, input1.data(),
                    frameCount * sizeof(float));
        input->mBuffers[0].mDataByteSize = frameCount * sizeof(float);
        input->mBuffers[1].mDataByteSize = frameCount * sizeof(float);
        return noErr;
      };

      AudioUnitRenderActionFlags flags = 0;
      const AUAudioUnitStatus status =
          render(&flags, nullptr, kFrames, 0,
                 reinterpret_cast<AudioBufferList *>(&output), nullptr, pull);
      if (status != noErr) {
        std::fprintf(stderr, "direct render failed at block %u: %d\n", block,
                     (int)status);
        [unit deallocateRenderResources];
        return 5;
      }
      ++renderedBlocks;
      for (AUAudioFrameCount frame = 0; frame < kFrames; ++frame) {
        outputEnergy += (double)output0[frame] * output0[frame];
        outputEnergy += (double)output1[frame] * output1[frame];
        outputPeak = std::max(outputPeak, std::fabs(output0[frame]));
        outputPeak = std::max(outputPeak, std::fabs(output1[frame]));
      }
      rendered0.insert(rendered0.end(), output0.begin(), output0.end());
      rendered1.insert(rendered1.end(), output1.begin(), output1.end());
    }
    [unit deallocateRenderResources];
    const double outputRms = std::sqrt(outputEnergy / (2.0 * kFrames * kBlocks));
    const double inputRms = std::sqrt(inputEnergy / (2.0 * kFrames * kBlocks));
    std::printf("direct AU test: rendered=%u blocks input_rms=%.6f output_rms=%.6f output_peak=%.6f\n",
                renderedBlocks, inputRms, outputRms, outputPeak);
    if (argc == 3 && std::strcmp(argv[1], "--wav") == 0) {
      if (!writeWav(argv[2], rendered0, rendered1)) {
        std::fprintf(stderr, "could not write WAV: %s\n", argv[2]);
        return 7;
      }
      std::printf("wrote WAV: %s (%zu stereo frames)\n", argv[2], rendered0.size());
    }
    if (renderedBlocks != kBlocks || !std::isfinite(outputRms) || outputRms <= 1.0e-7) {
      std::fprintf(stderr, "direct AU test failed: silent or invalid output\n");
      return 6;
    }
    return 0;
  }
}
