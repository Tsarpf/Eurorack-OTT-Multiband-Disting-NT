#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioToolbox/AUAudioUnit.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vocoder_audio_engine.h"

namespace {

static constexpr unsigned int kMaxRenderFrames = 4096;

static AudioStreamBasicDescription monoFloatFormat() {
  AudioStreamBasicDescription format = {};
  format.mSampleRate = kVocoderRuntimeSampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
  format.mFramesPerPacket = 1;
  format.mChannelsPerFrame = 1;
  format.mBytesPerFrame = sizeof(float);
  format.mBytesPerPacket = sizeof(float);
  format.mBitsPerChannel = 32;
  return format;
}

static void configureLabel(UILabel *label, NSString *text) {
  label.text = text;
  label.textColor = [UIColor colorWithWhite:0.90 alpha:1.0];
  label.font = [UIFont systemFontOfSize:15.0 weight:UIFontWeightMedium];
}

} // namespace

class RemoteControlServer {
public:
  explicit RemoteControlServer(VocoderAudioEngine *audio) : audio_(audio), server_(-1), stopping_(false) {}
  ~RemoteControlServer() { stop(); }
  void start() {
    if (server_ >= 0) return;
    server_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_ < 0) return;
    int reuse = 1;
    setsockopt(server_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(47821);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 || listen(server_, 4) < 0) {
      close(server_); server_ = -1; return;
    }
    worker_ = std::thread([this] { serve(); });
  }
  void stop() {
    stopping_ = true;
    if (server_ >= 0) { shutdown(server_, SHUT_RDWR); close(server_); server_ = -1; }
    if (worker_.joinable()) worker_.join();
  }
private:
  static std::string runAUTest() {
    __block AUAudioUnit *unit = nil;
    __block NSError *initError = nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    AudioComponentDescription desc = {kAudioUnitType_Effect,'VCDR','TSPF', 0, 0};
    dispatch_async(dispatch_get_main_queue(), ^{
      [AUAudioUnit instantiateWithComponentDescription:desc options:0 completionHandler:^(AUAudioUnit *audioUnit, NSError *error) {
        unit = audioUnit;
        initError = error;
        dispatch_semaphore_signal(done);
      }];
    });
    dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC);
    if (dispatch_semaphore_wait(done, deadline) != 0) return "{\"ok\":false,\"error\":\"timed out waiting for AU instantiation\"}";
    if (!unit) {
      NSString *message = initError.localizedDescription ?: @"AUAudioUnit returned nil";
      return std::string("{\"ok\":false,\"error\":\"") + message.UTF8String + "\"}";
    }
    NSError *resourceError = nil;
    const BOOL ok = [unit allocateRenderResourcesAndReturnError:&resourceError];
    [unit deallocateRenderResources];
    if (!ok) {
      NSString *message = resourceError.localizedDescription ?: @"allocateRenderResources failed";
      return std::string("{\"ok\":false,\"error\":\"") + message.UTF8String + "\"}";
    }
    return "{\"ok\":true,\"message\":\"AU instantiated and render resources allocated\"}";
  }

  static int parameterForName(const std::string &name) {
    if (name == "depth") return kDepth;
    if (name == "width") return kBandWidth;
    if (name == "formant") return kFormant;
    if (name == "attack") return kAttack;
    if (name == "release" || name == "decay") return kRelease;
    if (name == "wet") return kWet;
    return -1;
  }
  std::string state() const {
    char body[512];
    std::snprintf(body, sizeof(body), "{\"running\":%s,\"route\":\"%s\",\"sampleRate\":48000,\"parameters\":{\"depth\":%d,\"width\":%d,\"formant\":%d,\"attack\":%d,\"release\":%d,\"wet\":%d}}", audio_->isRunning() ? "true" : "false", audio_->routeName(), audio_->parameter(kDepth), audio_->parameter(kBandWidth), audio_->parameter(kFormant), audio_->parameter(kAttack), audio_->parameter(kRelease), audio_->parameter(kWet));
    return body;
  }
  static std::string queryValue(const std::string &request, const char *key) {
    const std::string needle = std::string(key) + "=";
    const size_t at = request.find(needle);
    if (at == std::string::npos) return {};
    size_t end = request.find_first_of(" &\r\n", at + needle.size());
    return request.substr(at + needle.size(), end == std::string::npos ? std::string::npos : end - at - needle.size());
  }
  void respond(int client, int code, const std::string &body) {
    const char *status = code == 200 ? "200 OK" : "400 Bad Request";
    std::string response = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    send(client, response.data(), response.size(), 0);
  }
  void handle(int client, const std::string &request) {
    if (request.rfind("GET /state", 0) == 0) { respond(client, 200, state()); return; }
    if (request.rfind("GET /au-test", 0) == 0) { respond(client, 200, runAUTest()); return; }
    if (request.rfind("POST /start", 0) == 0) { dispatch_async(dispatch_get_main_queue(), ^{ audio_->start(); }); respond(client, 200, "{\"accepted\":true,\"action\":\"start\"}"); return; }
    if (request.rfind("POST /stop", 0) == 0) { dispatch_async(dispatch_get_main_queue(), ^{ audio_->stop(); }); respond(client, 200, "{\"accepted\":true,\"action\":\"stop\"}"); return; }
    if (request.rfind("POST /parameter", 0) == 0) {
      const int parameter = parameterForName(queryValue(request, "name"));
      const std::string value = queryValue(request, "value");
      if (parameter < 0 || value.empty()) { respond(client, 400, "{\"error\":\"use name=<parameter>&value=<number>\"}"); return; }
      audio_->setParameter(parameter, std::atoi(value.c_str()));
      respond(client, 200, "{\"accepted\":true}"); return;
    }
    respond(client, 400, "{\"error\":\"unknown endpoint\"}");
  }
  void serve() {
    while (!stopping_ && server_ >= 0) {
      const int client = accept(server_, nullptr, nullptr);
      if (client < 0) continue;
      char buffer[2048] = {};
      const ssize_t count = recv(client, buffer, sizeof(buffer) - 1, 0);
      if (count > 0) handle(client, std::string(buffer, static_cast<size_t>(count)));
      close(client);
    }
  }
  VocoderAudioEngine *audio_;
  int server_;
  volatile bool stopping_;
  std::thread worker_;
};

VocoderAudioEngine::VocoderAudioEngine()
    : runtime_(kVocoderRuntimeSampleRate), audioUnit_(nullptr), running_(false),
      routeName_{}, input_{}, output_{} {
  std::strncpy(routeName_, "not running", sizeof(routeName_) - 1);
  runtime_.initialize();
}

VocoderAudioEngine::~VocoderAudioEngine() { stop(); }

bool VocoderAudioEngine::start() {
  if (running_) {
    return true;
  }
  if (!runtime_.isInitialized()) {
    return false;
  }

  AVAudioSession *session = [AVAudioSession sharedInstance];
  NSError *error = nil;
  if (![session setCategory:AVAudioSessionCategoryPlayAndRecord
                         mode:AVAudioSessionModeMeasurement
                      options:AVAudioSessionCategoryOptionMixWithOthers |
                              AVAudioSessionCategoryOptionAllowBluetooth
                        error:&error]) {
    return false;
  }
  [session setPreferredSampleRate:kVocoderRuntimeSampleRate error:&error];
  [session setPreferredIOBufferDuration:128.0 / kVocoderRuntimeSampleRate
                                  error:&error];
  if (![session setActive:YES error:&error]) {
    return false;
  }

  AudioComponentDescription description = {};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_RemoteIO;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (!component || AudioComponentInstanceNew(component, &audioUnit_) != noErr) {
    audioUnit_ = nullptr;
    return false;
  }

  UInt32 enabled = 1;
  if (AudioUnitSetProperty(audioUnit_, kAudioOutputUnitProperty_EnableIO,
                           kAudioUnitScope_Input, 1, &enabled,
                           sizeof(enabled)) != noErr) {
    stop();
    return false;
  }

  AURenderCallbackStruct callback = {};
  callback.inputProc = [](void *refCon, AudioUnitRenderActionFlags *flags,
                          const AudioTimeStamp *timestamp,
                          UInt32 busNumber, UInt32 frameCount,
                          AudioBufferList *ioData) -> OSStatus {
    return static_cast<OSStatus>(VocoderAudioEngine::renderCallback(
        refCon, flags, timestamp, busNumber, frameCount, ioData));
  };
  callback.inputProcRefCon = this;
  if (AudioUnitSetProperty(audioUnit_, kAudioUnitProperty_SetRenderCallback,
                           kAudioUnitScope_Input, 0, &callback,
                           sizeof(callback)) != noErr) {
    stop();
    return false;
  }

  const AudioStreamBasicDescription format = monoFloatFormat();
  if (AudioUnitSetProperty(audioUnit_, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Output, 1, &format,
                           sizeof(format)) != noErr ||
      AudioUnitSetProperty(audioUnit_, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Input, 0, &format,
                           sizeof(format)) != noErr ||
      AudioUnitInitialize(audioUnit_) != noErr ||
      AudioOutputUnitStart(audioUnit_) != noErr) {
    stop();
    return false;
  }

  running_ = true;
  AVAudioSessionRouteDescription *route = session.currentRoute;
  NSString *name = route.inputs.firstObject.portName;
  if (!name) {
    name = route.outputs.firstObject.portName;
  }
  if (name) {
    std::strncpy(routeName_, name.UTF8String, sizeof(routeName_) - 1);
    routeName_[sizeof(routeName_) - 1] = '\0';
  }
  return true;
}

void VocoderAudioEngine::stop() {
  if (audioUnit_) {
    if (running_) {
      AudioOutputUnitStop(audioUnit_);
    }
    AudioUnitUninitialize(audioUnit_);
    AudioComponentInstanceDispose(audioUnit_);
    audioUnit_ = nullptr;
  }
  running_ = false;
  std::strncpy(routeName_, "not running", sizeof(routeName_) - 1);
}

long VocoderAudioEngine::renderCallback(void *refCon, unsigned int *actionFlags,
                                        const void *timestamp,
                                        unsigned int busNumber,
                                        unsigned int frameCount, void *ioData) {
  return static_cast<VocoderAudioEngine *>(refCon)->render(
      actionFlags, timestamp, busNumber, frameCount, ioData);
}

long VocoderAudioEngine::render(unsigned int *actionFlags, const void *timestamp,
                                unsigned int busNumber, unsigned int frameCount,
                                void *opaqueIoData) {
  auto *ioData = static_cast<AudioBufferList *>(opaqueIoData);
  if (!ioData || frameCount > kMaxRenderFrames || !audioUnit_) {
    if (ioData) {
      for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; ++buffer) {
        std::memset(ioData->mBuffers[buffer].mData, 0,
                    ioData->mBuffers[buffer].mDataByteSize);
      }
    }
    return noErr;
  }

  AudioBufferList input = {};
  input.mNumberBuffers = 1;
  input.mBuffers[0].mNumberChannels = 1;
  input.mBuffers[0].mDataByteSize = frameCount * sizeof(float);
  input.mBuffers[0].mData = input_;
  AudioUnitRenderActionFlags flags = 0;
  const OSStatus status = AudioUnitRender(
      audioUnit_, &flags, static_cast<const AudioTimeStamp *>(timestamp), 1,
      frameCount, &input);
  if (status != noErr) {
    for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; ++buffer) {
      std::memset(ioData->mBuffers[buffer].mData, 0,
                  ioData->mBuffers[buffer].mDataByteSize);
    }
    return status;
  }

  runtime_.process(input_, output_, frameCount);
  for (UInt32 buffer = 0; buffer < ioData->mNumberBuffers; ++buffer) {
    AudioBuffer &out = ioData->mBuffers[buffer];
    if (!out.mData) {
      continue;
    }
    float *samples = static_cast<float *>(out.mData);
    if (out.mNumberChannels <= 1) {
      std::memcpy(samples, output_, frameCount * sizeof(float));
    } else {
      for (UInt32 frame = 0; frame < frameCount; ++frame) {
        for (UInt32 channel = 0; channel < out.mNumberChannels; ++channel) {
          samples[frame * out.mNumberChannels + channel] = output_[frame];
        }
      }
    }
  }
  return noErr;
}

static VocoderAudioEngine *gAudio = nullptr;
static RemoteControlServer *gRemote = nullptr;

@interface VocoderViewController : UIViewController
@end

@implementation VocoderViewController {
  UILabel *_status;
  UIButton *_startButton;
  NSMutableArray<UISlider *> *_sliders;
  NSMutableArray<UILabel *> *_valueLabels;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithWhite:0.055 alpha:1.0];
  _sliders = [NSMutableArray array];
  _valueLabels = [NSMutableArray array];

  UIScrollView *scroll = [[UIScrollView alloc] initWithFrame:self.view.bounds];
  scroll.autoresizingMask = UIViewAutoresizingFlexibleWidth |
                            UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:scroll];

  UILabel *title = [[UILabel alloc] initWithFrame:CGRectMake(22, 28, 340, 32)];
  configureLabel(title, @"iPhone Vocoder");
  title.font = [UIFont systemFontOfSize:27.0 weight:UIFontWeightBold];
  [scroll addSubview:title];

  UILabel *subtitle =
      [[UILabel alloc] initWithFrame:CGRectMake(23, 62, 350, 38)];
  configureLabel(subtitle, @"Ableton-style 40-band bank • input is carrier + modulator");
  subtitle.numberOfLines = 2;
  subtitle.font = [UIFont systemFontOfSize:13.0];
  subtitle.textColor = [UIColor colorWithWhite:0.68 alpha:1.0];
  [scroll addSubview:subtitle];

  _status = [[UILabel alloc] initWithFrame:CGRectMake(23, 105, 350, 24)];
  configureLabel(_status, @"Stopped • USB or built-in audio route");
  _status.textColor = [UIColor colorWithRed:0.35 green:0.85 blue:0.75 alpha:1.0];
  [scroll addSubview:_status];

  _startButton = [UIButton buttonWithType:UIButtonTypeSystem];
  _startButton.frame = CGRectMake(23, 138, 150, 42);
  _startButton.layer.cornerRadius = 10.0;
  _startButton.backgroundColor = [UIColor colorWithRed:0.12 green:0.45 blue:0.40 alpha:1.0];
  [_startButton setTitle:@"Start audio" forState:UIControlStateNormal];
  [_startButton setTitleColor:[UIColor whiteColor] forState:UIControlStateNormal];
  [_startButton addTarget:self action:@selector(toggleAudio)
         forControlEvents:UIControlEventTouchUpInside];
  [scroll addSubview:_startButton];

  [self addSliderTo:scroll title:@"Depth" parameter:kDepth min:0 max:200 y:208];
  [self addSliderTo:scroll title:@"Width" parameter:kBandWidth min:0 max:200 y:274];
  [self addSliderTo:scroll title:@"Formant" parameter:kFormant min:-360 max:360 y:340];
  [self addSliderTo:scroll title:@"Attack (ms)" parameter:kAttack min:1 max:500 y:406];
  [self addSliderTo:scroll title:@"Decay (ms)" parameter:kRelease min:1 max:1000 y:472];
  [self addSliderTo:scroll title:@"Wet" parameter:kWet min:0 max:100 y:538];

  UILabel *note = [[UILabel alloc] initWithFrame:CGRectMake(23, 615, 350, 70)];
  configureLabel(note, @"Use headphones or an external USB interface to avoid feedback.\nThe audio session is fixed at 48 kHz to match the shared NT DSP bank.");
  note.numberOfLines = 3;
  note.font = [UIFont systemFontOfSize:12.0];
  note.textColor = [UIColor colorWithWhite:0.58 alpha:1.0];
  [scroll addSubview:note];
  scroll.contentSize = CGSizeMake(self.view.bounds.size.width, 720);
}

- (void)addSliderTo:(UIScrollView *)scroll
              title:(NSString *)title
          parameter:(int)parameter
                 min:(float)minimum
                 max:(float)maximum
                   y:(CGFloat)y {
  UILabel *label = [[UILabel alloc] initWithFrame:CGRectMake(23, y, 120, 23)];
  configureLabel(label, title);
  [scroll addSubview:label];
  UILabel *value = [[UILabel alloc] initWithFrame:CGRectMake(300, y, 65, 23)];
  configureLabel(value, @"");
  value.textAlignment = NSTextAlignmentRight;
  value.textColor = [UIColor colorWithRed:0.45 green:0.90 blue:0.80 alpha:1.0];
  [scroll addSubview:value];

  UISlider *slider = [[UISlider alloc] initWithFrame:CGRectMake(20, y + 24, 345, 30)];
  slider.minimumValue = minimum;
  slider.maximumValue = maximum;
  slider.value = gAudio->parameter(parameter);
  slider.tag = parameter;
  [slider addTarget:self action:@selector(sliderChanged:)
   forControlEvents:UIControlEventValueChanged];
  [scroll addSubview:slider];
  [_sliders addObject:slider];
  [_valueLabels addObject:value];
  value.text = [NSString stringWithFormat:@"%d", (int)slider.value];
}

- (void)sliderChanged:(UISlider *)slider {
  const int parameter = (int)slider.tag;
  const int value = (int)lrintf(slider.value);
  gAudio->setParameter(parameter, value);
  NSUInteger index = [_sliders indexOfObject:slider];
  if (index != NSNotFound) {
    _valueLabels[index].text = [NSString stringWithFormat:@"%d", value];
  }
}

- (void)toggleAudio {
  if (gAudio->isRunning()) {
    gAudio->stop();
    [_startButton setTitle:@"Start audio" forState:UIControlStateNormal];
    _status.text = @"Stopped • USB or built-in audio route";
  } else if (gAudio->start()) {
    [_startButton setTitle:@"Stop audio" forState:UIControlStateNormal];
    NSString *route = [NSString stringWithUTF8String:gAudio->routeName()];
    _status.text = [NSString stringWithFormat:@"Running • %@", route ?: @"audio"];
  } else {
    _status.text = @"Could not start audio (check microphone permission/route)";
  }
}

- (void)dealloc {
}

@end

@interface VocoderAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation VocoderAppDelegate
- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
  gAudio = new VocoderAudioEngine();
  gRemote = new RemoteControlServer(gAudio);
  gRemote->start();
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = [[VocoderViewController alloc] init];
  [self.window makeKeyAndVisible];
  return YES;
}
@end

static int runCommandLineAUTest() {
  __block AUAudioUnit *unit = nil;
  __block NSError *error = nil;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  AudioComponentDescription desc = {kAudioUnitType_Effect, 'VCDR', 'TSPF', 0, 0};
  [AUAudioUnit instantiateWithComponentDescription:desc options:0 completionHandler:^(AUAudioUnit *audioUnit, NSError *e) {
    unit = audioUnit; error = e; dispatch_semaphore_signal(done);
  }];
  if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 8LL * NSEC_PER_SEC)) != 0) {
    std::fprintf(stderr, "AU self-test: timeout\n"); return 2;
  }
  if (!unit) {
    std::fprintf(stderr, "AU self-test: instantiate failed: %s\n", error.localizedDescription.UTF8String ?: "unknown");
    return 3;
  }
  NSError *resourceError = nil;
  if (![unit allocateRenderResourcesAndReturnError:&resourceError]) {
    std::fprintf(stderr, "AU self-test: allocate failed: %s\n", resourceError.localizedDescription.UTF8String ?: "unknown");
    return 4;
  }
  std::printf("AU self-test: instantiate and allocate succeeded\n");
  [unit deallocateRenderResources];
  return 0;
}

int main(int argc, char *argv[]) {
  @autoreleasepool {
    if (argc > 1 && std::strcmp(argv[1], "--au-self-test") == 0) return runCommandLineAUTest();
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass([VocoderAppDelegate class]));
  }
}
