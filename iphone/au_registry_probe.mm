#import <AVFAudio/AVAudioUnitComponent.h>
#import <AudioToolbox/AUAudioUnit.h>
#import <Foundation/Foundation.h>

#include <cstdio>

static void printDescription(const char *label, AudioComponentDescription desc) {
  std::printf("%s type=%08x subtype=%08x manufacturer=%08x flags=%08x mask=%08x\n",
              label, desc.componentType, desc.componentSubType,
              desc.componentManufacturer, desc.componentFlags,
              desc.componentFlagsMask);
}

static int probe(const char *label, AudioComponentDescription desc) {
  std::printf("=== %s ===\n", label);
  AVAudioUnitComponentManager *manager =
      [AVAudioUnitComponentManager sharedAudioUnitComponentManager];
  NSArray<AVAudioUnitComponent *> *matches =
      [manager componentsMatchingDescription:desc];
  std::printf("manager matches=%lu\n", (unsigned long)matches.count);
  for (AVAudioUnitComponent *component in matches) {
    AudioComponentDescription found = component.audioComponentDescription;
    printDescription("  component", found);
    std::printf("  name=%s manufacturer=%s type=%s version=%lu sandbox=%s midiIn=%s midiOut=%s\n",
                component.name.UTF8String ?: "", component.manufacturerName.UTF8String ?: "",
                component.typeName.UTF8String ?: "", (unsigned long)component.version,
                component.sandboxSafe ? "yes" : "no",
                component.hasMIDIInput ? "yes" : "no",
                component.hasMIDIOutput ? "yes" : "no");
    std::printf("  passesAUVal=%s config=%s\n", component.passesAUVal ? "yes" : "no",
                component.configurationDictionary.description.UTF8String ?: "(null)");
  }

  AudioComponent component = AudioComponentFindNext(nullptr, &desc);
  std::printf("AudioComponentFindNext=%s\n", component ? "found" : "nil");
  __block AUAudioUnit *unit = nil;
  __block NSError *error = nil;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [AUAudioUnit instantiateWithComponentDescription:desc options:0
                                 completionHandler:^(AUAudioUnit *audioUnit, NSError *e) {
    unit = audioUnit;
    error = e;
    dispatch_semaphore_signal(done);
  }];
  if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 12LL * NSEC_PER_SEC)) != 0) {
    std::printf("instantiate timeout\n");
    return 2;
  }
  if (!unit) {
    std::printf("instantiate failed domain=%s code=%ld message=%s\n",
                error.domain.UTF8String ?: "", (long)error.code,
                error.localizedDescription.UTF8String ?: "unknown");
    return 3;
  }
  NSError *resourceError = nil;
  BOOL ok = [unit allocateRenderResourcesAndReturnError:&resourceError];
  std::printf("instantiate succeeded inputBusses=%lu outputBusses=%lu allocate=%s",
              (unsigned long)unit.inputBusses.count,
              (unsigned long)unit.outputBusses.count, ok ? "yes" : "no");
  if (!ok) {
    std::printf(" domain=%s code=%ld message=%s", resourceError.domain.UTF8String ?: "",
                (long)resourceError.code,
                resourceError.localizedDescription.UTF8String ?: "unknown");
  }
  std::printf("\n");
  [unit deallocateRenderResources];
  return ok ? 0 : 4;
}

int main() {
  @autoreleasepool {
    const AudioComponentDescription ours = {kAudioUnitType_Effect, 'VCDR', 'TSPF', 0, 0};
    const AudioComponentDescription loopy = {kAudioUnitType_MusicEffect, 'lpau', 'atpx', 0, 0};
    const int oursResult = probe("ours", ours);
    const int loopyResult = probe("Loopy Pro installed AU", loopy);
    std::printf("summary ours=%d loopy=%d\n", oursResult, loopyResult);
    return oursResult == 0 && loopyResult == 0 ? 0 : 1;
  }
}
