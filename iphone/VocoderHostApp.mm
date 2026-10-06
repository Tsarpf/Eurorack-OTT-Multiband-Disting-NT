#import <AudioToolbox/AUAudioUnitImplementation.h>
#import <AVFAudio/AVAudioFormat.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(VOCODER_HOST_DAEMON)
@interface VocoderAudioUnit : AUAudioUnit
@end
#endif

namespace {

static constexpr UInt32 kHostSampleRate = 48000;
static constexpr UInt32 kHostFrames = 256;
static constexpr unsigned kHostBlocks = 64;
static constexpr int kHostPort = 47822;

struct TwoBufferList {
  UInt32 mNumberBuffers;
  AudioBuffer mBuffers[2];
};

static std::string jsonString(NSString *value) {
  if (!value) return "null";
  NSData *data = [NSJSONSerialization dataWithJSONObject:@[value]
                                                    options:0 error:nil];
  if (!data) return "\"\"";
  std::string encoded(static_cast<const char *>(data.bytes), data.length);
  if (encoded.size() >= 2) return encoded.substr(1, encoded.size() - 2);
  return encoded;
}

static NSData *makeWavData(const std::vector<float> &left,
                           const std::vector<float> &right) {
  if (left.size() != right.size()) return nil;
  const uint32_t channels = 2;
  const uint32_t sampleRate = kHostSampleRate;
  const uint32_t bytesPerSample = sizeof(float);
  const uint32_t blockAlign = channels * bytesPerSample;
  const uint32_t byteRate = sampleRate * blockAlign;
  const uint32_t dataSize = static_cast<uint32_t>(left.size() * blockAlign);
  const uint32_t riffSize = 36 + dataSize;
  const uint16_t format = 3; // IEEE-754 float
  const uint16_t channelCount = channels;
  const uint16_t bitCount = 32;
  const uint16_t align16 = static_cast<uint16_t>(blockAlign);
  const uint32_t fmtSize = 16;

  NSMutableData *data = [NSMutableData dataWithCapacity:44 + dataSize];
  auto append = [&](const void *bytes, NSUInteger length) {
    [data appendBytes:bytes length:length];
  };
  append("RIFF", 4); append(&riffSize, 4); append("WAVE", 4);
  append("fmt ", 4); append(&fmtSize, 4); append(&format, 2);
  append(&channelCount, 2); append(&sampleRate, 4); append(&byteRate, 4);
  append(&align16, 2); append(&bitCount, 2); append("data", 4);
  append(&dataSize, 4);
  for (size_t i = 0; i < left.size(); ++i) {
    append(&left[i], sizeof(float));
    append(&right[i], sizeof(float));
  }
  return data;
}

} // namespace

@interface VocoderHostEngine : NSObject
- (BOOL)prepare;
- (BOOL)isReady;
- (NSString *)lastError;
- (NSDictionary *)state;
- (NSDictionary *)renderTest;
- (BOOL)setParameterNamed:(NSString *)name value:(float)value;
- (NSData *)lastWav;
@end

@implementation VocoderHostEngine {
  AUAudioUnit *_unit;
  NSString *_lastError;
  NSData *_lastWav;
  BOOL _allocResources;
}

- (BOOL)prepare {
  @synchronized(self) {
    if (_unit && _allocResources) return YES;
    _lastError = nil;
    AudioComponentDescription description = {
        kAudioUnitType_Effect, 'VCDR', 'TSPF', 0, 0};
    __block AUAudioUnit *created = nil;
    __block NSError *initError = nil;
#if defined(VOCODER_HOST_DAEMON)
    // The daemon is the deterministic regression path.  It uses the same AU
    // class linked into the extension, avoiding LaunchServices and all private
    // container entitlements while the phone is locked.
    created = [[VocoderAudioUnit alloc] initWithComponentDescription:description
                                                               options:0
                                                                 error:&initError];
#else
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    [AUAudioUnit instantiateWithComponentDescription:description
                                              options:0
                                    completionHandler:^(AUAudioUnit *audioUnit,
                                                        NSError *error) {
      created = audioUnit;
      initError = error;
      dispatch_semaphore_signal(completed);
    }];
    if (dispatch_semaphore_wait(
            completed, dispatch_time(DISPATCH_TIME_NOW, 10LL * NSEC_PER_SEC)) != 0) {
      _lastError = @"timed out waiting for AUAudioUnit instantiation";
      return NO;
    }
#endif
    if (!created) {
      _lastError = initError.localizedDescription ?: @"AUAudioUnit returned nil";
      return NO;
    }
    NSError *resourceError = nil;
    if (![created allocateRenderResourcesAndReturnError:&resourceError]) {
      _lastError = resourceError.localizedDescription ?: @"allocateRenderResources failed";
      return NO;
    }
    _unit = created;
    _allocResources = YES;
    return YES;
  }
}

- (BOOL)isReady {
  @synchronized(self) { return _unit != nil && _allocResources; }
}

- (NSString *)lastError {
  @synchronized(self) { return _lastError ?: @""; }
}

- (NSDictionary *)state {
  @synchronized(self) {
    return @{ @"ready": @(_unit != nil && _allocResources),
              @"port": @(kHostPort),
              @"sampleRate": @(kHostSampleRate),
              @"lastError": _lastError ?: @"" };
  }
}

- (BOOL)setParameterNamed:(NSString *)name value:(float)value {
  if (![self prepare]) return NO;
  AUParameterAddress address = UINT32_MAX;
  if ([name isEqualToString:@"depth"]) address = 0;
  else if ([name isEqualToString:@"width"]) address = 1;
  else if ([name isEqualToString:@"formant"]) address = 2;
  else if ([name isEqualToString:@"attack"]) address = 3;
  else if ([name isEqualToString:@"release"] || [name isEqualToString:@"decay"]) address = 4;
  else if ([name isEqualToString:@"wet"]) address = 5;
  if (address == UINT32_MAX) return NO;
  @synchronized(self) {
    AUParameter *parameter = [_unit.parameterTree parameterWithAddress:address];
    if (!parameter) return NO;
    parameter.value = value;
    return YES;
  }
}

- (NSDictionary *)renderTest {
  if (![self prepare]) {
    return @{ @"ok": @NO, @"error": [self lastError] };
  }
  @synchronized(self) {
    AUInternalRenderBlock render = _unit.internalRenderBlock;
    if (!render) return @{ @"ok": @NO, @"error": @"AU render block is nil" };
    std::vector<float> input(kHostFrames);
    std::vector<float> left(kHostFrames), right(kHostFrames);
    std::vector<float> recordedLeft;
    std::vector<float> recordedRight;
    recordedLeft.reserve(kHostFrames * kHostBlocks);
    recordedRight.reserve(kHostFrames * kHostBlocks);
    double inputEnergy = 0.0;
    double outputEnergy = 0.0;
    float peak = 0.0f;
    for (unsigned block = 0; block < kHostBlocks; ++block) {
      for (UInt32 frame = 0; frame < kHostFrames; ++frame) {
        const double t = static_cast<double>(block * kHostFrames + frame) /
                         static_cast<double>(kHostSampleRate);
        input[frame] = 0.55f * std::sin(2.0 * 3.141592653589793 * 220.0 * t);
        inputEnergy += static_cast<double>(input[frame]) * input[frame];
      }
      TwoBufferList output = {};
      output.mNumberBuffers = 2;
      output.mBuffers[0].mNumberChannels = 1;
      output.mBuffers[0].mDataByteSize = kHostFrames * sizeof(float);
      output.mBuffers[0].mData = left.data();
      output.mBuffers[1].mNumberChannels = 1;
      output.mBuffers[1].mDataByteSize = kHostFrames * sizeof(float);
      output.mBuffers[1].mData = right.data();
      AURenderPullInputBlock pull = ^AUAudioUnitStatus(
          AudioUnitRenderActionFlags *flags, const AudioTimeStamp *timestamp,
          AUAudioFrameCount frameCount, NSInteger inputBus,
          AudioBufferList *inputData) {
        (void)flags; (void)timestamp; (void)inputBus;
        if (!inputData || inputData->mNumberBuffers < 2 || frameCount > kHostFrames) {
          return kAudioUnitErr_InvalidPropertyValue;
        }
        std::memcpy(inputData->mBuffers[0].mData, input.data(),
                    frameCount * sizeof(float));
        std::memcpy(inputData->mBuffers[1].mData, input.data(),
                    frameCount * sizeof(float));
        inputData->mBuffers[0].mDataByteSize = frameCount * sizeof(float);
        inputData->mBuffers[1].mDataByteSize = frameCount * sizeof(float);
        return noErr;
      };
      AudioUnitRenderActionFlags flags = 0;
      AudioTimeStamp timestamp = {};
      timestamp.mFlags = kAudioTimeStampSampleTimeValid;
      timestamp.mSampleTime = static_cast<Float64>(block * kHostFrames);
      const AUAudioUnitStatus status = render(
          &flags, &timestamp, kHostFrames, 0,
          reinterpret_cast<AudioBufferList *>(&output), nullptr, pull);
      if (status != noErr) {
        return @{ @"ok": @NO, @"error":
                  [NSString stringWithFormat:@"render returned %d", (int)status] };
      }
      for (UInt32 frame = 0; frame < kHostFrames; ++frame) {
        outputEnergy += static_cast<double>(left[frame]) * left[frame];
        outputEnergy += static_cast<double>(right[frame]) * right[frame];
        peak = std::max(peak, std::fabs(left[frame]));
        peak = std::max(peak, std::fabs(right[frame]));
      }
      recordedLeft.insert(recordedLeft.end(), left.begin(), left.end());
      recordedRight.insert(recordedRight.end(), right.begin(), right.end());
    }
    const NSUInteger frameCount = recordedLeft.size();
    const double inputRms = std::sqrt(inputEnergy / frameCount);
    const double outputRms =
        std::sqrt(outputEnergy / (2.0 * static_cast<double>(frameCount)));
    _lastWav = makeWavData(recordedLeft, recordedRight);
    NSString *path = [NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES).firstObject
        stringByAppendingPathComponent:@"vocoder-host-test.wav"];
    [_lastWav writeToFile:path atomically:YES];
    return @{ @"ok": @(_lastWav != nil && std::isfinite(outputRms) && outputRms > 1.0e-7),
              @"frames": @(frameCount),
              @"inputRMS": @(inputRms),
              @"outputRMS": @(outputRms),
              @"peak": @(peak),
              @"path": path ?: @"" };
  }
}

- (NSData *)lastWav {
  @synchronized(self) { return _lastWav; }
}

@end

class HostRemoteServer {
public:
  explicit HostRemoteServer(VocoderHostEngine *engine)
      : engine_(engine), server_(-1), stopping_(false) {}
  ~HostRemoteServer() { stop(); }

  void start() {
    if (server_ >= 0) return;
    server_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_ < 0) return;
    int reuse = 1;
    setsockopt(server_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(kHostPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
        listen(server_, 4) < 0) {
      close(server_); server_ = -1; return;
    }
    worker_ = std::thread([this] { serve(); });
  }

  void stop() {
    stopping_ = true;
    if (server_ >= 0) {
      shutdown(server_, SHUT_RDWR); close(server_); server_ = -1;
    }
    if (worker_.joinable()) worker_.join();
  }

private:
  static std::string json(NSDictionary *object) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:object options:0 error:nil];
    return data ? std::string(static_cast<const char *>(data.bytes), data.length)
                : "{\"ok\":false,\"error\":\"json encoding failed\"}";
  }

  static std::string queryValue(const std::string &request, const char *key) {
    const std::string needle = std::string(key) + "=";
    const size_t at = request.find(needle);
    if (at == std::string::npos) return {};
    const size_t end = request.find_first_of(" &\r\n", at + needle.size());
    return request.substr(at + needle.size(),
                          end == std::string::npos ? std::string::npos
                                                   : end - at - needle.size());
  }

  static bool sendAll(int client, const void *data, size_t length) {
    const char *bytes = static_cast<const char *>(data);
    while (length > 0) {
      const ssize_t sent = send(client, bytes, length, 0);
      if (sent <= 0) return false;
      bytes += sent; length -= static_cast<size_t>(sent);
    }
    return true;
  }

  void respond(int client, int code, const char *contentType,
               const void *body, size_t length) {
    const char *status = code == 200 ? "200 OK" : "400 Bad Request";
    std::string header = std::string("HTTP/1.1 ") + status +
        "\r\nContent-Type: " + contentType +
        "\r\nContent-Length: " + std::to_string(length) +
        "\r\nConnection: close\r\n\r\n";
    sendAll(client, header.data(), header.size());
    sendAll(client, body, length);
  }

  void handle(int client, const std::string &request) {
    if (request.rfind("GET /state", 0) == 0) {
      const std::string body = json([engine_ state]);
      respond(client, 200, "application/json", body.data(), body.size());
      return;
    }
    if (request.rfind("GET /au-test", 0) == 0 ||
        request.rfind("GET /render-test", 0) == 0) {
      const std::string body = json([engine_ renderTest]);
      respond(client, 200, "application/json", body.data(), body.size());
      return;
    }
    if (request.rfind("GET /render.wav", 0) == 0) {
      NSData *wav = [engine_ lastWav];
      if (!wav) {
        [engine_ renderTest];
        wav = [engine_ lastWav];
      }
      if (!wav) {
        const char *body = "{\"ok\":false,\"error\":\"no WAV available\"}";
        respond(client, 400, "application/json", body, std::strlen(body));
      } else {
        respond(client, 200, "audio/wav", wav.bytes, wav.length);
      }
      return;
    }
    if (request.rfind("POST /parameter", 0) == 0) {
      NSString *name = [NSString stringWithUTF8String:queryValue(request, "name").c_str()];
      const std::string value = queryValue(request, "value");
      const BOOL ok = name && !value.empty() &&
          [engine_ setParameterNamed:name value:std::strtof(value.c_str(), nullptr)];
      const std::string body = ok ? "{\"accepted\":true}" :
          "{\"ok\":false,\"error\":\"invalid parameter\"}";
      respond(client, ok ? 200 : 400, "application/json", body.data(), body.size());
      return;
    }
    const char *body = "{\"ok\":false,\"error\":\"unknown endpoint\"}";
    respond(client, 400, "application/json", body, std::strlen(body));
  }

  void serve() {
    while (!stopping_ && server_ >= 0) {
      const int client = accept(server_, nullptr, nullptr);
      if (client < 0) continue;
      char buffer[4096] = {};
      const ssize_t count = recv(client, buffer, sizeof(buffer) - 1, 0);
      if (count > 0) handle(client, std::string(buffer, static_cast<size_t>(count)));
      close(client);
    }
  }

  VocoderHostEngine *engine_;
  int server_;
  volatile bool stopping_;
  std::thread worker_;
};

static VocoderHostEngine *gHostEngine = nil;
static HostRemoteServer *gHostServer = nullptr;

// SpringBoard can create an app in a suspended state for background-capable
// launches, before UIKit delivers didFinishLaunching.  Start the loopback
// control plane before UIApplicationMain so a headless test can still observe
// the process; no UI activation or lock-screen bypass is involved.
__attribute__((constructor)) static void HostPreMainBootstrap() {
  @autoreleasepool {
    gHostEngine = [VocoderHostEngine new];
    gHostServer = new HostRemoteServer(gHostEngine);
    gHostServer->start();
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
      [gHostEngine prepare];
    });
  }
}

@interface VocoderHostViewController : UIViewController
@end

@implementation VocoderHostViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithWhite:0.055 alpha:1.0];
  UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(22, 42, 370, 42)];
  title.text = @"Vocoder AU Host";
  title.textColor = UIColor.whiteColor;
  title.font = [UIFont systemFontOfSize:28.0 weight:UIFontWeightBold];
  [self.view addSubview:title];
  UILabel *detail = [[UILabel alloc] initWithFrame:CGRectMake(24, 94, 360, 120)];
  detail.numberOfLines = 0;
  detail.textColor = [UIColor colorWithWhite:0.75 alpha:1.0];
  detail.font = [UIFont systemFontOfSize:15.0];
  detail.text = @"Remote host on port 47822\nGET /state\nGET /au-test\nGET /render.wav\nPOST /parameter?name=wet&value=80";
  [self.view addSubview:detail];
}
@end

@interface VocoderHostAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation VocoderHostAppDelegate
- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
  (void)application; (void)launchOptions;
  if (!gHostEngine) gHostEngine = [VocoderHostEngine new];
  if (!gHostServer) {
    gHostServer = new HostRemoteServer(gHostEngine);
    gHostServer->start();
  }
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
    [gHostEngine prepare];
  });
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = [VocoderHostViewController new];
  [self.window makeKeyAndVisible];
  return YES;
}
@end

#if defined(VOCODER_HOST_DAEMON)
int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  dispatch_main();
}
#else
int main(int argc, char *argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass([VocoderHostAppDelegate class]));
  }
}
#endif
