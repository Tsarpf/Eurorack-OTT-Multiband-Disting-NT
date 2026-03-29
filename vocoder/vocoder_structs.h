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

struct VocoderDSPState {
  float an_y1[2][kVocoderMaxBands];
  float an_y2[2][kVocoderMaxBands];
  float sy_y1[2][kVocoderMaxBands];
  float sy_y2[2][kVocoderMaxBands];
  float env[2][kVocoderMaxBands];
  float eAvg[2][kVocoderMaxBands];
  float cAvg[2][kVocoderMaxBands];
  float gainState[2][kVocoderMaxBands];
  float meters[kVocoderMaxBands];
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
  bool descriptorDirty;
};

struct _vocoderAlgorithm : public _NT_algorithm {
  _vocoderAlgorithm()
      : descriptor(nullptr), state(nullptr), activeBands(8), uiDirty(true) {
    controls.currentBandwidth = 50.0f;
    controls.targetBandwidth = 50.0f;
    controls.currentFormant = 0.0f;
    controls.targetFormant = 0.0f;
    controls.currentWet = 100.0f;
    controls.targetWet = 100.0f;
    controls.descriptorDirty = true;
  }

  VocoderDescriptor *descriptor;
  VocoderDSPState *state;
  VocoderControlState controls;
  int activeBands;
  bool uiDirty;
};

#endif // VOCODER_STRUCTS_H
