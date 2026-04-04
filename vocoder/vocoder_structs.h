#ifndef VOCODER_STRUCTS_H
#define VOCODER_STRUCTS_H

#include "batch_biquad.h"
#include "vocoder_dsp.h"
#include <distingnt/api.h>

struct VocoderDescriptor {
  int activeBands;
  float analysisFreq[kVocoderMaxBands];
  float synthesisFreq[kVocoderMaxBands];
  float synthesisBandGain[kVocoderMaxBands];
  float enhanceTarget[kVocoderMaxBands];
  // DF1 coefficients computed by vocoderCalculateBandpass
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
  // Batch biquad DF2T state (2 floats per filter vs 4 in old DF1)
  BatchBiquadState anState[2][kVocoderMaxBands];
  BatchBiquadState syState[2][kVocoderMaxBands];
  BatchBiquadState prevSyState[2][kVocoderMaxBands];

  // Batch biquad coefficients (opaque — accessed via batchBiquad* API)
  BatchBiquadCoeffs anCoeffs[kVocoderMaxBands];
  BatchBiquadCoeffs syCoeffs[kVocoderMaxBands];
  BatchBiquadCoeffs prevSyCoeffs[kVocoderMaxBands];

  // DF1-level synthesis coefficients for smoothing (plain floats)
  // Smoothed per-block, then converted to BatchBiquadCoeffs
  float sy_b0_smooth[kVocoderMaxBands];
  float sy_b2_smooth[kVocoderMaxBands];
  float sy_a1_smooth[kVocoderMaxBands];
  float sy_a2_smooth[kVocoderMaxBands];

  float sy_b0_target[kVocoderMaxBands];
  float sy_b2_target[kVocoderMaxBands];
  float sy_a1_target[kVocoderMaxBands];
  float sy_a2_target[kVocoderMaxBands];

  // Envelope follower state
  float env[2][kVocoderMaxBands];
  float eAvg[2][kVocoderMaxBands];
  float cAvg[2][kVocoderMaxBands];
  float envPeakHold[2][kVocoderMaxBands];
  float carrierPeakHold[2][kVocoderMaxBands];
  float gainTarget[2][kVocoderMaxBands];
  float gainState[2][kVocoderMaxBands];

  // Metering
  float meters[kVocoderMaxBands];
  float meterPeakHold[kVocoderMaxBands];

  // Level matching
  float dryAvg[2];
  float wetAvg[2];
  float wetMakeup[2];
  float wetMakeupTarget[2];
  float wetMakeupStep[2];
  float inputPeakSmoothed[2];
  float inputGuard[2];
  float outputGuard[2];
  float outputGuardTarget[2];
  float outputGuardStep[2];
  float dryPeakHold[2];
  float wetPeakHold[2];

  // Band gain smoothing
  float synthesisBandGainCurrent[kVocoderMaxBands];
  float bandwidthCompCurrent;

  // DC blocking
  float carrierDcX1[2];
  float carrierDcY1[2];
  float modDcX1[2];
  float modDcY1[2];

  // Crossfade
  int prevActiveBands;
  int synthesisXfadeRemaining;
  int synthesisXfadeTotal;

  // Phase counters
  int controlPhase;
  int levelPhase;
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
  bool synthesisDirty;
  bool synthesisCoeffSmoothing;
};

struct _vocoderAlgorithm : public _NT_algorithm {
  _vocoderAlgorithm()
      : descriptor(nullptr), state(nullptr), activeBands(8), uiDirty(true),
        leftEncoderControlsDecay(false), rightEncoderControlsGain(false),
        uiReleaseDisplay(100), uiWetDisplay(100), uiOutputGainDisplay(0) {
    controls.currentBandwidth = 50.0f;
    controls.targetBandwidth = 50.0f;
    controls.currentFormant = 0.0f;
    controls.targetFormant = 0.0f;
    controls.currentWet = 100.0f;
    controls.targetWet = 100.0f;
    controls.currentOutputGainDb = 0.0f;
    controls.targetOutputGainDb = 0.0f;
    controls.descriptorDirty = true;
    controls.synthesisDirty = true;
    controls.synthesisCoeffSmoothing = true;
  }

  VocoderDescriptor *descriptor;
  VocoderDSPState *state;
  VocoderControlState controls;
  int activeBands;
  bool uiDirty;
  bool leftEncoderControlsDecay;
  bool rightEncoderControlsGain;
  int uiReleaseDisplay;
  int uiWetDisplay;
  int uiOutputGainDisplay;
};

#endif // VOCODER_STRUCTS_H
