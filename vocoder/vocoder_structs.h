#ifndef VOCODER_STRUCTS_H
#define VOCODER_STRUCTS_H

#include "vocoder_dsp.h"
#include <distingnt/api.h>

struct VocoderDescriptor {
  int activeBands;
  float analysisFreq[kVocoderMaxBands];
  float synthesisFreq[kVocoderMaxBands];
  float enhanceTarget[kVocoderMaxBands];
  float an_b0[kVocoderMaxBands];
  float an_b2[kVocoderMaxBands];
  float an_a1[kVocoderMaxBands];
  float an_a2[kVocoderMaxBands];
  float sy_b0[kVocoderMaxBands];
  float sy_b2[kVocoderMaxBands];
  float sy_a1[kVocoderMaxBands];
  float sy_a2[kVocoderMaxBands];
  float bandwidthCompensation;
  float analysisQ;
  float synthesisQ;
};

struct VocoderDepthShape {
  float depthControl;
  float depthMix;
  float peakExponent;
  bool peakMode;
};

struct VocoderDSPState {
  float an_y1[2][kVocoderMaxBands];
  float an_y2[2][kVocoderMaxBands];
  float sy_y1[2][kVocoderMaxBands];
  float sy_y2[2][kVocoderMaxBands];
  float sy_b0_current[kVocoderMaxBands];
  float sy_b2_current[kVocoderMaxBands];
  float sy_a1_current[kVocoderMaxBands];
  float sy_a2_current[kVocoderMaxBands];
  float env[2][kVocoderMaxBands];
  float eAvg[2][kVocoderMaxBands];
  float cAvg[2][kVocoderMaxBands];
  float gainState[2][kVocoderMaxBands];
  float meters[kVocoderMaxBands];
  float dryAvg[2];
  float wetAvg[2];
  float wetMakeup[2];
  float inputGuard[2];
  float outputGuard[2];
  float mod_x1[2];
  float mod_x2[2];
  float car_x1[2];
  float car_x2[2];
};

struct VocoderControlState {
  float currentBandwidth;
  float targetBandwidth;
  float currentFormant;
  float targetFormant;
  float currentWet;
  float targetWet;
  float currentOutputGainDb;
  float targetOutputGainDb;
  bool descriptorDirty;
  bool synthesisCoeffSmoothing;
};

struct _vocoderAlgorithm : public _NT_algorithm {
  _vocoderAlgorithm()
      : descriptor(nullptr), state(nullptr), activeBands(8), uiDirty(true),
        rightEncoderControlsGain(false), uiWetDisplay(100),
        uiOutputGainDisplay(0) {
    controls.currentBandwidth = 50.0f;
    controls.targetBandwidth = 50.0f;
    controls.currentFormant = 0.0f;
    controls.targetFormant = 0.0f;
    controls.currentWet = 100.0f;
    controls.targetWet = 100.0f;
    controls.currentOutputGainDb = 0.0f;
    controls.targetOutputGainDb = 0.0f;
    controls.descriptorDirty = true;
    controls.synthesisCoeffSmoothing = true;
  }

  VocoderDescriptor *descriptor;
  VocoderDSPState *state;
  VocoderControlState controls;
  int activeBands;
  bool uiDirty;
  bool rightEncoderControlsGain;
  int uiWetDisplay;
  int uiOutputGainDisplay;
};

#endif // VOCODER_STRUCTS_H
