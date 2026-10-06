#import <AudioToolbox/AUAudioUnitImplementation.h>
#import <Foundation/Foundation.h>
#import <AVFAudio/AVAudioFormat.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "vocoder_runtime.h"

static constexpr AUParameterAddress kAuDepth = 0;
static constexpr AUParameterAddress kAuWidth = 1;
static constexpr AUParameterAddress kAuFormant = 2;
static constexpr AUParameterAddress kAuAttack = 3;
static constexpr AUParameterAddress kAuRelease = 4;
static constexpr AUParameterAddress kAuWet = 5;

static int runtimeParameterForAddress(AUParameterAddress address) {
  switch (address) {
  case kAuDepth:
    return kDepth;
  case kAuWidth:
    return kBandWidth;
  case kAuFormant:
    return kFormant;
  case kAuAttack:
    return kAttack;
  case kAuRelease:
    return kRelease;
  case kAuWet:
    return kWet;
  default:
    return -1;
  }
}

@interface VocoderAudioUnit : AUAudioUnit
@end

@implementation VocoderAudioUnit {
  AUAudioUnitBus *_inputBus;
  AUAudioUnitBus *_outputBus;
  AUAudioUnitBusArray *_inputBusses;
  AUAudioUnitBusArray *_outputBusses;
  AUParameterTree *_parameterTree;
  VocoderRuntime *_runtime;
  float _inputScratch[8192];
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                        options:(AudioComponentInstantiationOptions)options
                                          error:(NSError **)error {
  self = [super initWithComponentDescription:componentDescription
                                      options:options
                                        error:error];
  if (!self) {
    return nil;
  }

  AVAudioFormat *format =
      [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kVocoderRuntimeSampleRate
                                                       channels:2];
  _inputBus = [[AUAudioUnitBus alloc] initWithFormat:format error:error];
  _outputBus = [[AUAudioUnitBus alloc] initWithFormat:format error:error];
  if (!_inputBus || !_outputBus) {
    return nil;
  }
  _inputBus.name = @"Input (carrier + modulator)";
  _outputBus.name = @"Vocoder output";
  _inputBus.shouldAllocateBuffer = NO;
  _outputBus.shouldAllocateBuffer = NO;
  _inputBusses = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                         busType:AUAudioUnitBusTypeInput
                                                         busses:@[_inputBus]];
  _outputBusses = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                          busType:AUAudioUnitBusTypeOutput
                                                          busses:@[_outputBus]];

  _runtime = new VocoderRuntime(kVocoderRuntimeSampleRate);
  if (!_runtime->initialize()) {
    delete _runtime;
    _runtime = nullptr;
    return nil;
  }

  AUParameter *depth = [AUParameterTree createParameterWithIdentifier:@"depth"
                                                                  name:@"Depth"
                                                               address:kAuDepth
                                                                   min:0.0
                                                                   max:200.0
                                                                  unit:kAudioUnitParameterUnit_Percent
                                                              unitName:nil
                                                                 flags:kAudioUnitParameterFlag_IsWritable |
                                                                       kAudioUnitParameterFlag_IsReadable
                                                          valueStrings:nil
                                                   dependentParameters:nil];
  AUParameter *width = [AUParameterTree createParameterWithIdentifier:@"width"
                                                                  name:@"Width"
                                                               address:kAuWidth
                                                                   min:0.0
                                                                   max:200.0
                                                                  unit:kAudioUnitParameterUnit_Percent
                                                              unitName:nil
                                                                 flags:kAudioUnitParameterFlag_IsWritable |
                                                                       kAudioUnitParameterFlag_IsReadable
                                                          valueStrings:nil
                                                   dependentParameters:nil];
  AUParameter *formant = [AUParameterTree createParameterWithIdentifier:@"formant"
                                                                    name:@"Formant"
                                                                 address:kAuFormant
                                                                     min:-360.0
                                                                     max:360.0
                                                                    unit:kAudioUnitParameterUnit_RelativeSemiTones
                                                                unitName:nil
                                                                   flags:kAudioUnitParameterFlag_IsWritable |
                                                                         kAudioUnitParameterFlag_IsReadable
                                                            valueStrings:nil
                                                     dependentParameters:nil];
  AUParameter *attack = [AUParameterTree createParameterWithIdentifier:@"attack"
                                                                  name:@"Attack"
                                                               address:kAuAttack
                                                                   min:1.0
                                                                   max:500.0
                                                                  unit:kAudioUnitParameterUnit_Milliseconds
                                                              unitName:nil
                                                                 flags:kAudioUnitParameterFlag_IsWritable |
                                                                       kAudioUnitParameterFlag_IsReadable
                                                          valueStrings:nil
                                                   dependentParameters:nil];
  AUParameter *release = [AUParameterTree createParameterWithIdentifier:@"release"
                                                                    name:@"Decay"
                                                                 address:kAuRelease
                                                                     min:1.0
                                                                     max:1000.0
                                                                    unit:kAudioUnitParameterUnit_Milliseconds
                                                                unitName:nil
                                                                   flags:kAudioUnitParameterFlag_IsWritable |
                                                                         kAudioUnitParameterFlag_IsReadable
                                                            valueStrings:nil
                                                     dependentParameters:nil];
  AUParameter *wet = [AUParameterTree createParameterWithIdentifier:@"wet"
                                                                name:@"Wet"
                                                             address:kAuWet
                                                                 min:0.0
                                                                 max:100.0
                                                                unit:kAudioUnitParameterUnit_Percent
                                                            unitName:nil
                                                               flags:kAudioUnitParameterFlag_IsWritable |
                                                                     kAudioUnitParameterFlag_IsReadable
                                                        valueStrings:nil
                                                 dependentParameters:nil];
  _parameterTree = [AUParameterTree createTreeWithChildren:@[depth, width, formant,
                                                               attack, release, wet]];
  VocoderRuntime *runtime = _runtime;
  _parameterTree.implementorValueObserver = ^(AUParameter *parameter,
                                               AUValue value) {
    const int target = runtimeParameterForAddress(parameter.address);
    if (target >= 0) {
      runtime->setParameter(target, static_cast<int>(value));
    }
  };
  depth.value = runtime->parameter(kDepth);
  width.value = runtime->parameter(kBandWidth);
  formant.value = runtime->parameter(kFormant);
  attack.value = runtime->parameter(kAttack);
  release.value = runtime->parameter(kRelease);
  wet.value = runtime->parameter(kWet);
  self.maximumFramesToRender = 4096;
  return self;
}

- (void)dealloc {
  delete _runtime;
}

- (AUAudioUnitBusArray *)inputBusses { return _inputBusses; }
- (AUAudioUnitBusArray *)outputBusses { return _outputBusses; }
- (AUParameterTree *)parameterTree { return _parameterTree; }

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)error {
  if (![super allocateRenderResourcesAndReturnError:error]) {
    return NO;
  }
  return YES;
}

- (void)deallocateRenderResources {
  [super deallocateRenderResources];
}

- (AUInternalRenderBlock)internalRenderBlock {
  VocoderRuntime *runtime = _runtime;
  float *scratch = _inputScratch;
  return ^AUAudioUnitStatus(AudioUnitRenderActionFlags *actionFlags,
                            const AudioTimeStamp *timestamp,
                            AUAudioFrameCount frameCount,
                            NSInteger outputBusNumber,
                            AudioBufferList *outputData,
                            const AURenderEvent *events,
                            AURenderPullInputBlock pullInputBlock) {
    (void)timestamp;
    (void)outputBusNumber;
    if (!runtime || !outputData || !pullInputBlock || frameCount > 4096 ||
        outputData->mNumberBuffers == 0) {
      return kAudioUnitErr_TooManyFramesToProcess;
    }

    for (const AURenderEvent *event = events; event; event = event->head.next) {
      if (event->head.eventType == AURenderEventParameter ||
          event->head.eventType == AURenderEventParameterRamp) {
        const int target = runtimeParameterForAddress(event->parameter.parameterAddress);
        if (target >= 0) {
          runtime->setParameter(target, static_cast<int>(event->parameter.value));
        }
      }
    }

    // AudioBufferList declares mBuffers[1] in the SDK; use storage for both
    // non-interleaved stereo buffers instead of writing past that inline
    // member (which corrupted the stack during host instantiation).
    struct TwoBufferList {
      UInt32 mNumberBuffers;
      AudioBuffer mBuffers[2];
    } input = {};
    input.mNumberBuffers = 2;
    input.mBuffers[0].mNumberChannels = 1;
    input.mBuffers[0].mDataByteSize = frameCount * sizeof(float);
    input.mBuffers[0].mData = scratch;
    input.mBuffers[1].mNumberChannels = 1;
    input.mBuffers[1].mDataByteSize = frameCount * sizeof(float);
    input.mBuffers[1].mData = scratch + 4096;
    AUAudioUnitStatus status = pullInputBlock(actionFlags, timestamp, frameCount,
                                               0, reinterpret_cast<AudioBufferList *>(&input));
    if (status != noErr) {
      return status;
    }
    const float *in = static_cast<const float *>(input.mBuffers[0].mData);
    float *out = static_cast<float *>(outputData->mBuffers[0].mData);
    if (!in || !out) {
      return kAudioUnitErr_InvalidPropertyValue;
    }
    runtime->process(in, out, frameCount);
    if (outputData->mNumberBuffers > 1) {
      for (UInt32 buffer = 1; buffer < outputData->mNumberBuffers; ++buffer) {
        if (!outputData->mBuffers[buffer].mData) {
          continue;
        }
        std::memcpy(outputData->mBuffers[buffer].mData, out,
                    frameCount * sizeof(float));
      }
    }
    return noErr;
  };
}

@end

@interface VocoderAudioUnitFactory : NSObject <AUAudioUnitFactory>
@end

@implementation VocoderAudioUnitFactory

- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context {
  // AUAudioUnitFactory uses the extension context for the lifetime of the
  // host connection.  Completing it here would tear down that connection
  // before the host asks createAudioUnitWithComponentDescription:error:.
  (void)context;
}
- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error {
  return [[VocoderAudioUnit alloc] initWithComponentDescription:desc
                                                          options:0
                                                            error:error];
}
@end
