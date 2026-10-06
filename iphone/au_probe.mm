#import <UIKit/UIKit.h>
#import <AudioToolbox/AUAudioUnit.h>
#import <Foundation/Foundation.h>
#import <AVFAudio/AVAudioFormat.h>
#include <cstdio>
#include <cmath>
#include <cstring>

int main() {
  @autoreleasepool {
    AudioComponentDescription desc = {kAudioUnitType_Effect, 'VCDR', 'TSPF', 0, 0};
    __block AUAudioUnit *unit = nil;
    __block NSError *error = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [AUAudioUnit instantiateWithComponentDescription:desc options:0 completionHandler:^(AUAudioUnit *au, NSError *e) {
      unit = au; error = e; dispatch_semaphore_signal(done);
    }];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC)) != 0) {
      std::fprintf(stderr, "instantiate timeout\n"); return 2;
    }
    if (!unit) {
      std::fprintf(stderr, "instantiate failed: %s\n", error.localizedDescription.UTF8String ?: "unknown"); return 3;
    }
    NSError *resourceError = nil;
    if (![unit allocateRenderResourcesAndReturnError:&resourceError]) {
      std::fprintf(stderr, "allocate failed: %s\n", resourceError.localizedDescription.UTF8String ?: "unknown"); return 4;
    }
    std::printf("instantiate and allocate succeeded; input buses=%lu output buses=%lu\n", (unsigned long)unit.inputBusses.count, (unsigned long)unit.outputBusses.count);
    [unit deallocateRenderResources];
    return 0;
  }
}
