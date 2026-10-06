#import <Foundation/Foundation.h>
#include <dlfcn.h>
#import <objc/message.h>
#include <cstdio>
#include <cstring>
#include <stdbool.h>

typedef int (*SBSLaunchApplicationWithIdentifierFn)(CFStringRef, bool);
typedef UInt32 (*SBSLaunchApplicationWithIdentifierAndLaunchOptionsFn)(CFStringRef, CFDictionaryRef, bool);
typedef CFStringRef (*SBSApplicationLaunchingErrorStringFn)(UInt32);

static void printSbsResult(void *handle, UInt32 result) {
  if (result == 0) return;
  auto errorString = reinterpret_cast<SBSApplicationLaunchingErrorStringFn>(dlsym(handle, "SBSApplicationLaunchingErrorString"));
  CFStringRef description = errorString ? errorString(result) : nullptr;
  if (description) {
    char buffer[512] = {0};
    if (CFStringGetCString(description, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
      std::fprintf(stderr, "SBS launch failed: %u (%s)\n", result, buffer);
      return;
    }
  }
  std::fprintf(stderr, "SBS launch failed: %u\n", result);
}

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 2 || argc > 3) {
      std::fprintf(stderr, "usage: ai-launchapp <bundle-id> [--suspended|--background|--unlock|--prompt-unlock|--fbs-background]\n");
      return 64;
    }
    CFStringRef identifier = CFStringCreateWithCString(kCFAllocatorDefault, argv[1], kCFStringEncodingUTF8);
    void *handle = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_GLOBAL);
    if (!handle) { std::fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    const char *mode = argc == 3 ? argv[2] : "";
    const bool suspended = std::strcmp(mode, "--suspended") == 0;
    const bool background = std::strcmp(mode, "--background") == 0;
    const bool unlock = std::strcmp(mode, "--unlock") == 0;
    const bool promptUnlock = std::strcmp(mode, "--prompt-unlock") == 0;
    const bool fbsBackground = std::strcmp(mode, "--fbs-background") == 0;
    if (argc == 3 && !suspended && !background && !unlock && !promptUnlock && !fbsBackground) {
      std::fprintf(stderr, "unknown mode: %s\n", mode);
      CFRelease(identifier);
      dlclose(handle);
      return 64;
    }
    if (suspended) {
      auto launch = reinterpret_cast<SBSLaunchApplicationWithIdentifierFn>(dlsym(handle, "SBSLaunchApplicationWithIdentifier"));
      if (!launch) { std::fprintf(stderr, "launch API unavailable: %s\n", dlerror()); return 2; }
      int result = launch(identifier, true);
      if (result != 0) std::fprintf(stderr, "suspended launch failed: %d\n", result);
      CFRelease(identifier);
      dlclose(handle);
      return result;
    }
    if (background || unlock || promptUnlock) {
      // SpringBoardServices exposes the same launch path used by system
      // clients.  The option is deliberately explicit: it asks SpringBoard
      // to present the unlock UI before activating the requested app; it does
      // not attempt to supply or bypass a passcode.
      auto launchWithOptions = reinterpret_cast<SBSLaunchApplicationWithIdentifierAndLaunchOptionsFn>(
          dlsym(handle, "SBSLaunchApplicationWithIdentifierAndLaunchOptions"));
      if (!launchWithOptions) {
        std::fprintf(stderr, "launch-with-options API unavailable: %s\n", dlerror());
        CFRelease(identifier);
        dlclose(handle);
        return 2;
      }
      NSDictionary *options = background
          ? @{
              @"activateSuspended": @YES,
              @"__ActivateSuspended": @YES,
              @"__LaunchOrigin": @"com.tsarpf.vocoderbridge"
            }
          : promptUnlock
          ? @{
              @"unlockDevice": @YES,
              @"promptUnlockDevice": @YES,
              @"__UnlockDevice": @YES,
              @"__PromptUnlockDevice": @YES,
              @"__LaunchOrigin": @"com.tsarpf.vocoderbridge"
            }
          : @{
              @"unlockDevice": @YES,
              @"__UnlockDevice": @YES,
              @"__LaunchOrigin": @"com.tsarpf.vocoderbridge"
            };
      UInt32 result = launchWithOptions(identifier, (__bridge CFDictionaryRef)options, false);
      printSbsResult(handle, result);
      CFRelease(identifier);
      dlclose(handle);
      return result == 0 ? 0 : (int)result;
    }

    Class serviceClass = NSClassFromString(@"FBSSystemService");
    SEL sharedSelector = NSSelectorFromString(@"sharedService");
    if (serviceClass && [serviceClass respondsToSelector:sharedSelector]) {
      id service = ((id (*)(id, SEL))objc_msgSend)((id)serviceClass, sharedSelector);
      SEL openSelector = NSSelectorFromString(@"openApplication:options:withResult:");
      if (service && [service respondsToSelector:openSelector]) {
        NSString *bundleID = [NSString stringWithUTF8String:argv[1]];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSError *openError = nil;
        void (^completion)(NSError *) = ^(NSError *e) { openError = e; dispatch_semaphore_signal(done); };
        id options = nil;
        if (fbsBackground) {
          options = @{
              @"activateSuspended": @YES,
              @"__ActivateSuspended": @YES,
              @"__LaunchOrigin": @"com.tsarpf.vocoderbridge",
              @"__Actions": @[]
          };
        }
        ((void (*)(id, SEL, id, id, id))objc_msgSend)(service, openSelector, bundleID, options, completion);
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));
        if (openError) {
          std::fprintf(stderr, "FBS launch failed: %s\n", openError.localizedDescription.UTF8String ?: "unknown");
          CFRelease(identifier);
          dlclose(handle);
          return 3;
        }
        CFRelease(identifier);
        dlclose(handle);
        return 0;
      }
    }
    auto launch = reinterpret_cast<SBSLaunchApplicationWithIdentifierFn>(dlsym(handle, "SBSLaunchApplicationWithIdentifier"));
    if (!launch) { std::fprintf(stderr, "launch API unavailable: %s\n", dlerror()); return 2; }
    int result = launch(identifier, false);
    if (result != 0) std::fprintf(stderr, "launch failed: %d\n", result);
    CFRelease(identifier);
    dlclose(handle);
    return result;
  }
}
