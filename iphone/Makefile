ROOT := ..
SDK_ROOT := $(abspath $(ROOT)/distingnt_api)
CMSIS_DSP := $(abspath $(ROOT)/CMSIS-DSP)
CMSIS_CORE := $(abspath $(ROOT)/CMSIS_6)

# The defaults match the WSL setup used to build the first iPhone binary.  On
# another workstation, point THEOS at a Theos checkout containing an iPhoneOS
# SDK (or override IOS_SDK and IOS_TOOLCHAIN directly).
THEOS ?= /tmp/theos
IOS_SDK ?= $(THEOS)/sdks/iPhoneOS16.5.sdk
IOS_TOOLCHAIN ?= $(THEOS)/toolchain/linux/iphone/bin
IOS_MIN ?= 16.1
IOS_CC := $(IOS_TOOLCHAIN)/clang
IOS_CXX := $(IOS_TOOLCHAIN)/clang++
IOS_LDID := $(IOS_TOOLCHAIN)/ldid

HOST_CXX ?= c++
HOST_CC ?= cc
BUILD ?= build

INCLUDES := -I$(SDK_ROOT)/include -I$(CMSIS_DSP)/Include \
	-I$(CMSIS_DSP)/PrivateInclude -I$(CMSIS_CORE)/CMSIS/Core/Include \
	-I$(CURDIR)
HOST_CPPFLAGS := -D__GNUC_PYTHON__ $(INCLUDES)
HOST_CXXFLAGS := -std=gnu++17 -O2 $(HOST_CPPFLAGS)
HOST_CFLAGS := -O2 -D__GNUC_PYTHON__ -I$(CMSIS_DSP)/Include \
	-I$(CMSIS_DSP)/PrivateInclude -I$(CMSIS_CORE)/CMSIS/Core/Include

IOS_COMMON := -target arm64-apple-ios$(IOS_MIN) -isysroot $(IOS_SDK) \
	-miphoneos-version-min=$(IOS_MIN) -O2 -D__GNUC_PYTHON__ $(INCLUDES)
IOS_CXXFLAGS := $(IOS_COMMON) -stdlib=libc++ -fblocks
IOS_LINKFLAGS := -target arm64-apple-ios$(IOS_MIN) -isysroot $(IOS_SDK) \
	-miphoneos-version-min=$(IOS_MIN) -stdlib=libc++

HOST_CMSIS := $(BUILD)/host-df2t.o $(BUILD)/host-df2t-init.o
IOS_CMSIS := $(BUILD)/ios-df2t.o $(BUILD)/ios-df2t-init.o
HOST_ENGINE := $(BUILD)/host-embed.o $(BUILD)/host-runtime.o
IOS_ENGINE := $(BUILD)/ios-embed.o $(BUILD)/ios-runtime.o

.PHONY: all host-test ios-cli ios-app ipa release host-app host-ipa host-release host-daemon au-probe au-probe-ipa au-direct-probe au-registry-probe clean

APP_VERSION ?= 0.3.1
APP_BUILD ?= 10
HOST_VERSION ?= 0.1.1
HOST_BUILD ?= 4

all: host-test ios-cli ios-app ipa host-ipa

host-test: $(BUILD)/vocoder-cli
	$<

$(BUILD):
	mkdir -p $@

$(BUILD)/host-embed.o: vocoder_embed.cpp | $(BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) -c $< -o $@

$(BUILD)/host-runtime.o: vocoder_runtime.cpp vocoder_runtime.h | $(BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) -c $< -o $@

$(BUILD)/host-cli.o: vocoder_cli.cpp vocoder_runtime.h | $(BUILD)
	$(HOST_CXX) $(HOST_CXXFLAGS) -c $< -o $@

$(BUILD)/host-df2t.o: $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_f32.c | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/host-df2t-init.o: $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_init_f32.c | $(BUILD)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(BUILD)/vocoder-cli: $(HOST_ENGINE) $(BUILD)/host-cli.o $(HOST_CMSIS)
	$(HOST_CXX) $(HOST_CXXFLAGS) $^ -o $@

$(BUILD)/ios-embed.o: vocoder_embed.cpp | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -c $< -o $@

$(BUILD)/ios-runtime.o: vocoder_runtime.cpp vocoder_runtime.h | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -c $< -o $@

$(BUILD)/ios-cli.o: vocoder_cli.cpp vocoder_runtime.h | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -c $< -o $@

$(BUILD)/ios-df2t.o: $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_f32.c | $(BUILD)
	$(IOS_CC) $(IOS_COMMON) -c $< -o $@

$(BUILD)/ios-df2t-init.o: $(CMSIS_DSP)/Source/FilteringFunctions/arm_biquad_cascade_df2T_init_f32.c | $(BUILD)
	$(IOS_CC) $(IOS_COMMON) -c $< -o $@

$(BUILD)/vocoder-cli-ios: $(IOS_ENGINE) $(BUILD)/ios-cli.o $(IOS_CMSIS)
	$(IOS_CXX) $(IOS_LINKFLAGS) $^ -lc++ -o $@
	$(IOS_LDID) -S $@

ios-cli: $(BUILD)/vocoder-cli-ios

$(BUILD)/ios-app.o: VocoderApp.mm vocoder_audio_engine.h vocoder_runtime.h | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -fobjc-arc -c $< -o $@

$(BUILD)/VocoderBridge: $(IOS_ENGINE) $(BUILD)/ios-app.o $(IOS_CMSIS)
	$(IOS_CXX) $(IOS_LINKFLAGS) $^ -framework UIKit -framework AVFoundation \
		-framework AudioToolbox -framework Foundation -lc++ -o $@
	$(IOS_LDID) -SVocoderBridge.entitlements $@

ios-app: $(BUILD)/VocoderBridge

$(BUILD)/ios-host.o: VocoderHostApp.mm | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -fobjc-arc -c $< -o $@

$(BUILD)/VocoderHost: $(BUILD)/ios-host.o
	$(IOS_CXX) $(IOS_LINKFLAGS) $^ -framework UIKit -framework AudioToolbox \
		-framework Foundation -framework AVFAudio -lc++ -o $@
	$(IOS_LDID) -SVocoderHost.entitlements $@

host-app: $(BUILD)/VocoderHost

$(BUILD)/ios-host-daemon.o: VocoderHostApp.mm | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -DVOCODER_HOST_DAEMON -fobjc-arc -c $< -o $@

$(BUILD)/VocoderHostDaemon: $(IOS_ENGINE) $(BUILD)/ios-au.o $(BUILD)/ios-host-daemon.o $(IOS_CMSIS)
	$(IOS_CXX) $(IOS_LINKFLAGS) $^ -framework UIKit -framework AudioToolbox \
		-framework Foundation -framework AVFAudio -lc++ -o $@
	$(IOS_LDID) -S $@

host-daemon: $(BUILD)/VocoderHostDaemon


$(BUILD)/au-probe.o: au_probe.mm | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -fobjc-arc -fapplication-extension -c $< -o $@

$(BUILD)/au-probe: $(BUILD)/au-probe.o
	$(IOS_CXX) $(IOS_LINKFLAGS) $^ -framework UIKit -framework AudioToolbox -framework Foundation -framework AVFAudio -lc++ -o $@
	$(IOS_LDID) -S $@

au-probe: $(BUILD)/au-probe

$(BUILD)/au-registry-probe.o: au_registry_probe.mm | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -fobjc-arc -fapplication-extension -c $< -o $@

$(BUILD)/au-registry-probe: $(BUILD)/au-registry-probe.o
	$(IOS_CXX) $(IOS_LINKFLAGS) $^ -framework AudioToolbox -framework Foundation -framework AVFAudio -lc++ -o $@
	$(IOS_LDID) -SVocoderHost.entitlements $@

au-registry-probe: $(BUILD)/au-registry-probe

$(BUILD)/au-direct-probe.o: au_direct_probe.mm | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -fobjc-arc -fapplication-extension -c $< -o $@

$(BUILD)/au-direct-probe: $(IOS_ENGINE) $(BUILD)/ios-au.o $(BUILD)/au-direct-probe.o $(IOS_CMSIS)
	$(IOS_CXX) $(IOS_LINKFLAGS) $^ -framework AudioToolbox \
		-framework Foundation -framework AVFAudio -lc++ -o $@
	$(IOS_LDID) -S $@

au-direct-probe: $(BUILD)/au-direct-probe

$(BUILD)/AUProbe.ipa: $(BUILD)/au-probe
	rm -rf $(BUILD)/ProbePayload
	mkdir -p $(BUILD)/ProbePayload/AUProbe.app
	cp $(BUILD)/au-probe $(BUILD)/ProbePayload/AUProbe.app/AUProbe
	cp AUProbe-Info.plist $(BUILD)/ProbePayload/AUProbe.app/Info.plist
	$(IOS_LDID) -S $(BUILD)/ProbePayload/AUProbe.app/AUProbe
	(cd $(BUILD) && zip -qry AUProbe.ipa ProbePayload)

au-probe-ipa: $(BUILD)/AUProbe.ipa

$(BUILD)/ios-au.o: VocoderAU.mm vocoder_runtime.h | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -fobjc-arc -fapplication-extension -c $< -o $@

$(BUILD)/VocoderAU: $(IOS_ENGINE) $(BUILD)/ios-au.o $(IOS_CMSIS)
	$(IOS_CXX) $(IOS_LINKFLAGS) -fapplication-extension $^ -framework AudioToolbox \
		-framework Foundation -framework AVFAudio -lc++ -Wl,-e,_NSExtensionMain -o $@
	$(IOS_LDID) -S $@

$(BUILD)/VocoderBridge.ipa: $(BUILD)/VocoderBridge $(BUILD)/VocoderAU $(BUILD)/au-probe \
		VocoderBridge-Info.plist VocoderAU-Info.plist VocoderBridge.entitlements VocoderAU.entitlements
	rm -rf $(BUILD)/Payload
	mkdir -p $(BUILD)/Payload/VocoderBridge.app/PlugIns/VocoderAU.appex
	cp $(BUILD)/VocoderBridge $(BUILD)/Payload/VocoderBridge.app/VocoderBridge
	cp $(BUILD)/au-probe $(BUILD)/Payload/VocoderBridge.app/AUProbe
	cp VocoderBridge-Info.plist $(BUILD)/Payload/VocoderBridge.app/Info.plist
	cp $(BUILD)/VocoderAU $(BUILD)/Payload/VocoderBridge.app/PlugIns/VocoderAU.appex/VocoderAU
	cp VocoderAU-Info.plist $(BUILD)/Payload/VocoderBridge.app/PlugIns/VocoderAU.appex/Info.plist
	$(IOS_LDID) -SVocoderAU.entitlements $(BUILD)/Payload/VocoderBridge.app/PlugIns/VocoderAU.appex/VocoderAU
	$(IOS_LDID) -SVocoderBridge.entitlements $(BUILD)/Payload/VocoderBridge.app/VocoderBridge
	$(IOS_LDID) -S $(BUILD)/Payload/VocoderBridge.app/AUProbe
	(cd $(BUILD) && zip -qry VocoderBridge.ipa Payload)

ipa: $(BUILD)/VocoderBridge.ipa

$(BUILD)/VocoderBridge-$(APP_VERSION)-build$(APP_BUILD).ipa: $(BUILD)/VocoderBridge.ipa
	cp $< $@

release: $(BUILD)/VocoderBridge-$(APP_VERSION)-build$(APP_BUILD).ipa

$(BUILD)/VocoderHost.ipa: $(BUILD)/VocoderHost VocoderHost-Info.plist VocoderHost.entitlements
	rm -rf $(BUILD)/HostStage
	mkdir -p $(BUILD)/HostStage/Payload/VocoderHost.app
	cp $(BUILD)/VocoderHost $(BUILD)/HostStage/Payload/VocoderHost.app/VocoderHost
	cp VocoderHost-Info.plist $(BUILD)/HostStage/Payload/VocoderHost.app/Info.plist
	$(IOS_LDID) -SVocoderHost.entitlements $(BUILD)/HostStage/Payload/VocoderHost.app/VocoderHost
	rm -f $(BUILD)/VocoderHost.ipa
	(cd $(BUILD)/HostStage && zip -qry ../VocoderHost.ipa Payload)

host-ipa: $(BUILD)/VocoderHost.ipa

$(BUILD)/VocoderHost-$(HOST_VERSION)-build$(HOST_BUILD).ipa: $(BUILD)/VocoderHost.ipa
	cp $< $@

host-release: $(BUILD)/VocoderHost-$(HOST_VERSION)-build$(HOST_BUILD).ipa

clean:
	rm -rf $(BUILD)

$(BUILD)/launchapp.o: launchapp.mm | $(BUILD)
	$(IOS_CXX) $(IOS_CXXFLAGS) -fobjc-arc -c $< -o $@

$(BUILD)/ai-launchapp: $(BUILD)/launchapp.o launchapp.entitlements
	$(IOS_CXX) $(IOS_LINKFLAGS) $(BUILD)/launchapp.o -framework Foundation -lc++ -o $@
	$(IOS_LDID) -Slaunchapp.entitlements $@

.PHONY: launchapp
launchapp: $(BUILD)/ai-launchapp
