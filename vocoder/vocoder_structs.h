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
  BatchBiquadCoeffs analysisCoeffs[kVocoderMaxBands];
  BatchBiquadCoeffs synthesisCoeffs[kVocoderMaxBands];
  float bandwidthCompensation;
  float analysisQ;
  float synthesisQ;
};

struct VocoderDSPState {
  // Independent state for every section of every analysis/synthesis band.
  BatchBiquadState anState[2][kVocoderMaxBands];
  BatchBiquadState syState[2][kVocoderMaxBands];

  // Batch biquad coefficients (opaque — accessed via batchBiquad* API)
  BatchBiquadCoeffs anCoeffs[kVocoderMaxBands];
  BatchBiquadCoeffs syCoeffs[kVocoderMaxBands];

  // Envelope follower state
  float env[2][kVocoderMaxBands];
  float gainState[2][kVocoderMaxBands];

  // Metering
  float meters[kVocoderMaxBands];
  float meterPeakHold[kVocoderMaxBands];

  // Overload protection (attenuation only, no automatic makeup gain).
  float inputPeakSmoothed[2];
  float inputGuard[2];
  float outputGuard[2];
  float outputGuardTarget[2];
  float outputGuardStep[2];
  float wetPeakHold[2];

  // Band gain smoothing
  float synthesisBandGainCurrent[kVocoderMaxBands];
  float bandwidthCompCurrent;

  // DC blocking
  float carrierDcX1[2];
  float carrierDcY1[2];
  float modDcX1[2];
  float modDcY1[2];

  // Phase counters
  int controlPhase;

  // Tracks whether the preceding callback actually had a distinct right
  // output, rather than merely having a stereo toggle enabled.
  bool stereoOutputWasActive;
  bool carrierStereoWasActive;
  bool modulatorStereoWasActive;
};

struct VocoderCachedCoeffs {
  float attackMix;
  float releaseMix;
  float synthesisCoeffMix;
  float synthesisScalarMix;
  float gainRiseMix;
  float gainFallMix;
  float masterScale;
  float dcBlockR;
  float meterRiseMix;
  float meterFallMix;
  float inputPeakRiseMix;
  float inputPeakFallMix;
  float inputGuardAttackMix;
  float inputGuardReleaseMix;
  float guardAttackMix;
  float guardReleaseMix;
  // Keys used to detect staleness
  int   lastN;
  float lastSampleRate;
  int   lastAttack;
  int   lastRelease;
  int   lastActiveBands;
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
        uiReleaseDisplay(100), uiWetDisplay(100), uiOutputGainDisplay(0),
        blockCoeffs{} {
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
  // Appended last — must not be reordered or removed (NT memory layout rule)
  VocoderCachedCoeffs blockCoeffs;
  // Build targets incrementally so transcendental work never fills a callback.
  VocoderDescriptor pendingDescriptor;
  int pendingBand = 0;
  bool buildingDescriptor = false;
  float pendingRatio = 1.0f;
  float pendingMin = 20.0f;
  float pendingStep = 1.0f;
  int coefficientWorkPhase = 0;
  bool bankInitialized = false;
};

#endif // VOCODER_STRUCTS_H
