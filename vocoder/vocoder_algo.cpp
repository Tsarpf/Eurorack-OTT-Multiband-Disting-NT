#include "batch_biquad.h"
#include "vocoder_dsp.h"
#include "vocoder_parameters.h"
#include "vocoder_structs.h"
#include <distingnt/api.h>
#include <distingnt/serialisation.h>
#include <new>
#include <string.h>

bool draw(_NT_algorithm *self);
uint32_t hasCustomUi(_NT_algorithm *self);
void customUi(_NT_algorithm *self, const _NT_uiData &data);
void setupUi(_NT_algorithm *self, _NT_float3 &pots);

static float bandwidthTableValue(const float *table, float x) {
  const float clamped = vocoderClamp(x, 0.0f, 1.0f) * 4.0f;
  int index = (int)clamped;
  if (index > 3) {
    index = 3;
  }
  const float frac = clamped - (float)index;
  return vocoderLerp(table[index], table[index + 1], frac);
}

static float computeBandwidthCompensation(int bandCount, float bandwidthPct) {
  static const float table8[5] = {1.00f, 0.71f, 0.50f, 0.48f, 0.48f};
  static const float table40[5] = {0.86f, 0.45f, 0.24f, 0.23f, 0.23f};

  const float bw = vocoderClamp(bandwidthPct / 100.0f, 0.0f, 1.0f);
  const float t8 = bandwidthTableValue(table8, bw);
  const float t40 = bandwidthTableValue(table40, bw);
  const float mix = vocoderClamp(((float)bandCount - 8.0f) / 32.0f, 0.0f, 1.0f);
  return vocoderLerp(t8, t40, mix);
}

static VocoderDepthShape computeDepthShape(float depthPct) {
  VocoderDepthShape shape = {};
  shape.depthControl = vocoderClamp(depthPct / 100.0f, 0.0f, 8.0f);
  shape.depthMix = vocoderClamp(shape.depthControl, 0.0f, 1.0f);
  shape.peakMode = shape.depthControl > 1.0f;
  shape.peakExponent = 1.0f + 1.2f * (shape.depthControl - 1.0f);
  return shape;
}

static float computeDepthGain(const VocoderDepthShape &shape,
                              float envAmplitude, float envNorm) {
  const float presence = vocoderClamp(envAmplitude * 5.0f, 0.0f, 1.0f);
  const float clampedEnv = vocoderClamp(envNorm, 0.25f, 4.0f);
  if (!shape.peakMode) {
    const float classicShape = presence * clampedEnv;
    return vocoderLerp(1.0f, classicShape, shape.depthMix);
  }

  const float x = clampedEnv;
  const int lowerExponent = (int)shape.peakExponent; // 1..9 for Depth 100..800
  float lowerPower = x;
  for (int exponent = 1; exponent < lowerExponent; ++exponent) {
    lowerPower *= x;
  }
  const float upperPower = lowerPower * x;
  return presence * vocoderLerp(lowerPower, upperPower,
                                shape.peakExponent - (float)lowerExponent);
}

static void clearDescriptor(VocoderDescriptor &descriptor) {
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.activeBands = 8;
  descriptor.bandwidthCompensation = 1.0f;
  descriptor.analysisQ = 1.0f;
  descriptor.synthesisQ = 1.0f;
}

static void clearState(VocoderDSPState &state) {
  memset(&state, 0, sizeof(state));
}

// Copy only the DF2T state floats: each channel's CMSIS instance must retain
// its own pState pointer.
static void synchroniseRightCarrierState(VocoderDSPState &state) {
  for (int band = 0; band < kVocoderMaxBands; ++band) {
    state.syState[1][band].state[0] = state.syState[0][band].state[0];
    state.syState[1][band].state[1] = state.syState[0][band].state[1];
  }
  state.carrierDcX1[1] = state.carrierDcX1[0];
  state.carrierDcY1[1] = state.carrierDcY1[0];
}

static void synchroniseRightModulatorState(VocoderDSPState &state) {
  for (int band = 0; band < kVocoderMaxBands; ++band) {
    state.anState[1][band].state[0] = state.anState[0][band].state[0];
    state.anState[1][band].state[1] = state.anState[0][band].state[1];
    state.env[1][band] = state.env[0][band];
    state.eAvg[1][band] = state.eAvg[0][band];
    state.gainState[1][band] = state.gainState[0][band];
  }
  state.modDcX1[1] = state.modDcX1[0];
  state.modDcY1[1] = state.modDcY1[0];
}

// A fully mono interval advances only channel 0. Before a distinct right
// output becomes available again, seed every per-channel state from the
// current left state so an older stereo interval cannot leak through.
static void synchroniseRightChannelState(VocoderDSPState &state) {
  synchroniseRightCarrierState(state);
  synchroniseRightModulatorState(state);

  state.dryAvg[1] = state.dryAvg[0];
  state.wetAvg[1] = state.wetAvg[0];
  state.wetMakeup[1] = state.wetMakeup[0];
  state.wetMakeupTarget[1] = state.wetMakeupTarget[0];
  state.wetMakeupStep[1] = state.wetMakeupStep[0];
  state.inputPeakSmoothed[1] = state.inputPeakSmoothed[0];
  state.inputGuard[1] = state.inputGuard[0];
  state.outputGuard[1] = state.outputGuard[0];
  state.outputGuardTarget[1] = state.outputGuardTarget[0];
  state.outputGuardStep[1] = state.outputGuardStep[0];
  state.dryPeakHold[1] = state.dryPeakHold[0];
  state.wetPeakHold[1] = state.wetPeakHold[0];
}

// Convert DF1 descriptor coefficients to batch biquad DF2T format and update
// the inst.pCoeffs pointer in each filter state.  On ARM/CMSIS-DSP the init
// function stores a raw pointer to the coefficients array; if we update
// anCoeffs[band] without also patching pCoeffs the ARM path reads stale/garbage
// coefficients (dangling pointer to whatever batchBiquadInit was last given).
static void syncAnalysisCoefficients(_vocoderAlgorithm *a) {
  VocoderDescriptor &d = *a->descriptor;
  VocoderDSPState &s = *a->state;
  for (int band = 0; band < d.activeBands; ++band) {
    s.anCoeffs[band] = batchBiquadFromDF1(d.an_b0[band], d.an_b2[band],
                                          d.an_a1[band], d.an_a2[band]);
    for (int ch = 0; ch < 2; ++ch) {
      // Use batchBiquadReseat (not batchBiquadInit) to refresh pCoeffs/pState
      // pointers without zeroing state.  batchBiquadInit calls the CMSIS init
      // which memsets the state buffer, causing audible zipper noise during a
      // live bandwidth sweep.  Reseat fixes both pointers safely.
      batchBiquadReseat(s.anState[ch][band], s.anCoeffs[band]);
    }
  }
}

static void syncSynthesisCoefficients(_vocoderAlgorithm *a) {
  VocoderDescriptor &d = *a->descriptor;
  VocoderDSPState &s = *a->state;
  for (int band = 0; band < kVocoderMaxBands; ++band) {
    s.syCoeffs[band] = batchBiquadFromDF1(d.sy_b0[band], d.sy_b2[band],
                                          d.sy_a1[band], d.sy_a2[band]);
    // Snap smooth values to match so the next smoothing pass starts from here,
    // not from zero or a stale position.  Without this, synthesisCoeffSmoothing
    // interpolates from zero on startup and from a wrong position on formant
    // changes — both produce glitches or screeching.
    s.sy_b0_smooth[band] = d.sy_b0[band];
    s.sy_b2_smooth[band] = d.sy_b2[band];
    s.sy_a1_smooth[band] = d.sy_a1[band];
    s.sy_a2_smooth[band] = d.sy_a2[band];
    s.sy_b0_target[band] = d.sy_b0[band];
    s.sy_b2_target[band] = d.sy_b2[band];
    s.sy_a1_target[band] = d.sy_a1[band];
    s.sy_a2_target[band] = d.sy_a2[band];
    for (int ch = 0; ch < 2; ++ch) {
      // Use batchBiquadReseat (not batchBiquadInit) — init zeros the synthesis
      // filter state, causing a click/pop whenever smoothing converges or
      // a band-count/freq change snaps to new coefficients instantly.
      batchBiquadReseat(s.syState[ch][band], s.syCoeffs[band]);
    }
  }
}

static void rebuildSynthesisDescriptor(_vocoderAlgorithm *a) {
  VocoderDescriptor &d = *a->descriptor;
  const float formantRatio = powf(2.0f, a->controls.currentFormant / 120.0f);
  const float sampleRate = (float)NT_globals.sampleRate;
  const float synthesisFloorHz = 30.0f;
  const float synthesisFadeStartHz = 20.0f;
  const float synthesisCeilingHz = 0.49f * sampleRate;
  const float synthesisHighFadeStartHz =
      synthesisCeilingHz < 20000.0f ? 0.85f * synthesisCeilingHz : 20000.0f;

  for (int i = 0; i < d.activeBands; ++i) {
    const float shiftedFreq = d.analysisFreq[i] * formantRatio;
    d.synthesisFreq[i] =
        vocoderClamp(shiftedFreq, synthesisFloorHz, synthesisCeilingHz);
    const float lowEdgeGain =
        vocoderClamp((shiftedFreq - synthesisFadeStartHz) /
                         (synthesisFloorHz - synthesisFadeStartHz),
                     0.0f, 1.0f);
    const float highEdgeGain =
        vocoderClamp((synthesisCeilingHz - shiftedFreq) /
                         (synthesisCeilingHz - synthesisHighFadeStartHz),
                     0.0f, 1.0f);
    d.synthesisBandGain[i] = lowEdgeGain * highEdgeGain;
    vocoderCalculateBandpass(d.synthesisFreq[i], d.synthesisQ, sampleRate,
                             d.sy_b0[i], d.sy_b2[i], d.sy_a1[i], d.sy_a2[i]);
  }

  a->uiDirty = true;
}

static void rebuildDescriptor(_vocoderAlgorithm *a) {
  VocoderDescriptor &d = *a->descriptor;
  clearDescriptor(d);

  const int bandCount = a->v[kBandCount] < 4
                            ? 4
                            : (a->v[kBandCount] > 40 ? 40 : a->v[kBandCount]);
  const float minFreq = (float)(a->v[kMinFreq] < 30 ? 30 : a->v[kMinFreq]);
  const float maxFreq =
      (float)(a->v[kMaxFreq] > minFreq ? a->v[kMaxFreq] : minFreq + 20);
  const float sampleRate = (float)NT_globals.sampleRate;
  const float bandwidthPct = a->controls.currentBandwidth;
  const float bandwidthNorm = bandwidthPct / 100.0f;
  const float bandwidthCurve = bandwidthNorm * bandwidthNorm;
  // Analysis runs at full sample rate now (batch processing makes decimation
  // less necessary — the batch biquad already amortizes overhead)
  const float analysisSampleRate = sampleRate;

  d.activeBands = bandCount;
  d.analysisQ = powf(40.0f, 1.0f - bandwidthCurve) * powf(0.7f, bandwidthCurve);
  d.synthesisQ = d.analysisQ * 0.85f + 1.0f;
  if (d.synthesisQ < 3.0f) {
    d.synthesisQ = 3.0f;
  }
  d.bandwidthCompensation =
      computeBandwidthCompensation(bandCount, bandwidthPct);

  const float step =
      bandCount > 1 ? powf(maxFreq / minFreq, 1.0f / (float)(bandCount - 1))
                    : 1.0f;

  for (int i = 0; i < bandCount; ++i) {
    const float f = minFreq * powf(step, (float)i);
    d.analysisFreq[i] = f;
    vocoderCalculateBandpass(f, d.analysisQ, analysisSampleRate, d.an_b0[i],
                             d.an_b2[i], d.an_a1[i], d.an_a2[i]);
  }

  rebuildSynthesisDescriptor(a);
  syncAnalysisCoefficients(a);

  a->activeBands = bandCount;
  a->uiDirty = true;
}

static void calculateRequirements(_NT_algorithmRequirements &req,
                                  const int32_t *) {
  req.numParameters = kNumParams;
  req.sram = sizeof(_vocoderAlgorithm) + sizeof(VocoderDescriptor);
  req.dtc = sizeof(VocoderDSPState);
  req.dram = 0;
  req.itc = 0;
}

static _NT_algorithm *construct(const _NT_algorithmMemoryPtrs &ptrs,
                                const _NT_algorithmRequirements &,
                                const int32_t *) {
  auto *a = new (ptrs.sram) _vocoderAlgorithm();
  a->parameters = parameters;
  a->parameterPages = &paramPages;
  a->descriptor =
      (VocoderDescriptor *)((uintptr_t)ptrs.sram + sizeof(_vocoderAlgorithm));
  a->state = (VocoderDSPState *)ptrs.dtc;

  clearDescriptor(*a->descriptor);
  clearState(*a->state);
  a->state->wetMakeup[0] = 1.0f;
  a->state->wetMakeup[1] = 1.0f;
  a->state->wetMakeupTarget[0] = 1.0f;
  a->state->wetMakeupTarget[1] = 1.0f;
  a->state->inputGuard[0] = 1.0f;
  a->state->inputGuard[1] = 1.0f;
  a->state->outputGuard[0] = 1.0f;
  a->state->outputGuard[1] = 1.0f;
  a->state->outputGuardTarget[0] = 1.0f;
  a->state->outputGuardTarget[1] = 1.0f;
  a->state->bandwidthCompCurrent = 1.0f;
  for (int band = 0; band < kVocoderMaxBands; ++band) {
    a->state->synthesisBandGainCurrent[band] = 1.0f;
  }

  // Initialise all filter states pointing at their corresponding persistent
  // coefficient structs (already zeroed by clearState above).  On ARM/CMSIS-DSP
  // batchBiquadInit stores a raw pCoeffs pointer, so it must point into the
  // long-lived state struct rather than a local variable.
  for (int band = 0; band < kVocoderMaxBands; ++band) {
    for (int ch = 0; ch < 2; ++ch) {
      batchBiquadInit(a->state->anState[ch][band], a->state->anCoeffs[band]);
      batchBiquadInit(a->state->syState[ch][band], a->state->syCoeffs[band]);
    }
  }

  a->controls.currentBandwidth = 50.0f;
  a->controls.targetBandwidth = 50.0f;
  a->controls.currentFormant = 0.0f;
  a->controls.targetFormant = 0.0f;
  a->controls.currentWet = 100.0f;
  a->controls.targetWet = 100.0f;
  a->controls.currentOutputGainDb = 0.0f;
  a->controls.targetOutputGainDb = 0.0f;
  a->uiReleaseDisplay = 100;
  a->uiWetDisplay = 100;
  a->uiOutputGainDisplay = 0;
  a->controls.descriptorDirty = true;
  a->controls.synthesisCoeffSmoothing = true;
  return a;
}

static void parameterChanged(_NT_algorithm *self, int parameter) {
  auto *a = (_vocoderAlgorithm *)self;
  switch (parameter) {
  case kBandWidth:
    a->controls.targetBandwidth = (float)self->v[kBandWidth];
    a->controls.descriptorDirty = true;
    break;
  case kFormant:
    a->controls.targetFormant = (float)self->v[kFormant];
    a->controls.synthesisDirty = true;
    break;
  case kWet:
    a->controls.targetWet = (float)self->v[kWet];
    a->uiWetDisplay = self->v[kWet];
    break;
  case kRelease:
    a->uiReleaseDisplay = self->v[kRelease];
    break;
  case kPreGain:
    a->uiOutputGainDisplay = self->v[kPreGain];
    a->controls.targetOutputGainDb = self->v[kPreGain] * 0.1f;
    break;
  case kBandCount:
  case kMinFreq:
  case kMaxFreq:
    a->controls.descriptorDirty = true;
    break;
  default:
    break;
  }
  a->uiDirty = true;
}

static bool hasDistinctStereoInput(const _NT_algorithm *self) {
  const bool carrierHasRight = self->v[kCarrierStereo] > 0 &&
                               self->v[kInCarrier] < kNT_lastBus;
  const bool modulatorHasRight = self->v[kModulatorStereo] > 0 &&
                                 self->v[kInModulator] < kNT_lastBus;
  return carrierHasRight || modulatorHasRight;
}

static int parameterString(_NT_algorithm *self, int parameter, int, char *buff) {
  if (parameter != kOutRight) {
    return 0;
  }

  const bool stereo = hasDistinctStereoInput(self);
  const char *text = !stereo ? "Same as left"
                     : self->v[kOut] < kNT_lastBus ? "Output + 1"
                                                   : "Same as left";
  strcpy(buff, text);
  return (int)strlen(text);
}

static void updateControlState(_vocoderAlgorithm *a) {
  constexpr float kControlSmoothing = 0.35f;
  const bool movingBandwidth = fabsf(a->controls.targetBandwidth -
                                     a->controls.currentBandwidth) > 0.001f;
  const bool movingFormant =
      fabsf(a->controls.targetFormant - a->controls.currentFormant) > 0.001f;

  a->controls.currentBandwidth =
      vocoderSmoothToward(a->controls.currentBandwidth,
                          a->controls.targetBandwidth, kControlSmoothing);
  a->controls.currentFormant = vocoderSmoothToward(
      a->controls.currentFormant, a->controls.targetFormant, kControlSmoothing);
  a->controls.currentWet = vocoderSmoothToward(
      a->controls.currentWet, a->controls.targetWet, kControlSmoothing);
  a->controls.currentOutputGainDb =
      vocoderSmoothToward(a->controls.currentOutputGainDb,
                          a->controls.targetOutputGainDb, kControlSmoothing);

  if (a->controls.descriptorDirty || a->controls.synthesisDirty ||
      movingBandwidth || movingFormant) {
    if (a->controls.descriptorDirty || movingBandwidth) {
      rebuildDescriptor(a);
    } else {
      rebuildSynthesisDescriptor(a);
    }
    if (movingBandwidth || movingFormant) {
      // Set targets; smoothing will interpolate towards them
      VocoderDescriptor &d = *a->descriptor;
      VocoderDSPState &s = *a->state;
      for (int band = 0; band < d.activeBands; ++band) {
        s.sy_b0_target[band] = d.sy_b0[band];
        s.sy_b2_target[band] = d.sy_b2[band];
        s.sy_a1_target[band] = d.sy_a1[band];
        s.sy_a2_target[band] = d.sy_a2[band];
      }
      a->controls.synthesisCoeffSmoothing = true;
    } else {
      syncSynthesisCoefficients(a);
      a->controls.synthesisCoeffSmoothing = false;
    }
    a->controls.descriptorDirty = movingBandwidth;
    a->controls.synthesisDirty = movingFormant;
  }
}

// Smooth synthesis coefficients toward target (called once per block, not per
// sample)
static void smoothSynthesisCoefficients(VocoderDSPState &s, int activeBands,
                                        float mix) {
  const float oneMinusMix = 1.0f - mix;
  for (int band = 0; band < activeBands; ++band) {
    s.sy_b0_smooth[band] =
        mix * s.sy_b0_smooth[band] + oneMinusMix * s.sy_b0_target[band];
    s.sy_b2_smooth[band] =
        mix * s.sy_b2_smooth[band] + oneMinusMix * s.sy_b2_target[band];
    s.sy_a1_smooth[band] =
        mix * s.sy_a1_smooth[band] + oneMinusMix * s.sy_a1_target[band];
    s.sy_a2_smooth[band] =
        mix * s.sy_a2_smooth[band] + oneMinusMix * s.sy_a2_target[band];

    s.syCoeffs[band] =
        batchBiquadFromDF1(s.sy_b0_smooth[band], s.sy_b2_smooth[band],
                           s.sy_a1_smooth[band], s.sy_a2_smooth[band]);

    for (int ch = 0; ch < 2; ++ch) {
      batchBiquadUpdateCoeffs(s.syState[ch][band], s.syCoeffs[band]);
    }
  }
}

static void computeBlockCoeffs(_vocoderAlgorithm *a, int N, float sampleRate) {
  VocoderCachedCoeffs &bc = a->blockCoeffs;
  const float blockRate = sampleRate / (float)N;
  const float meterControlSampleRate = sampleRate / 128.0f;

  bc.attackMix           = vocoderMixCoeffFromSeconds(blockRate, (float)bc.lastAttack  * 0.001f);
  bc.releaseMix          = vocoderMixCoeffFromSeconds(blockRate, (float)bc.lastRelease * 0.001f);
  bc.envAvgRiseMix       = vocoderMixCoeffFromSeconds(blockRate, 0.015f);
  bc.envAvgFallMix       = vocoderMixCoeffFromSeconds(blockRate, 0.05f);
  bc.synthesisCoeffMix   = vocoderMixCoeffFromSeconds(blockRate, 0.0015f);
  bc.synthesisScalarMix  = vocoderMixCoeffFromSeconds(blockRate, 0.0015f);
  bc.gainRiseMix         = vocoderMixCoeffFromSeconds(sampleRate, 0.0015f);
  bc.gainFallMix         = vocoderMixCoeffFromSeconds(sampleRate, 0.005f);
  bc.masterScale         = 8.0f / sqrtf((float)bc.lastActiveBands);
  bc.dcBlockR            = expf(-2.0f * 3.14159265359f * 30.0f / sampleRate);
  bc.levelAvgRiseMix     = vocoderMixCoeffFromSeconds(blockRate, 0.01f);
  bc.levelAvgFallMix     = vocoderMixCoeffFromSeconds(blockRate, 0.12f);
  bc.meterRiseMix        = vocoderMixCoeffFromSeconds(meterControlSampleRate, 0.01f);
  bc.meterFallMix        = vocoderMixCoeffFromSeconds(meterControlSampleRate, 0.08f);
  bc.makeupRiseMix       = vocoderMixCoeffFromSeconds(blockRate, 0.05f);
  bc.makeupFallMix       = vocoderMixCoeffFromSeconds(blockRate, 0.008f);
  bc.inputPeakRiseMix    = vocoderMixCoeffFromSeconds(sampleRate, 0.0005f);
  bc.inputPeakFallMix    = vocoderMixCoeffFromSeconds(sampleRate, 0.008f);
  bc.inputGuardAttackMix = vocoderMixCoeffFromSeconds(sampleRate, 0.00035f);
  bc.inputGuardReleaseMix= vocoderMixCoeffFromSeconds(sampleRate, 0.04f);
  bc.guardAttackMix      = vocoderMixCoeffFromSeconds(blockRate, 0.0002f);
  bc.guardReleaseMix     = vocoderMixCoeffFromSeconds(blockRate, 0.05f);

  bc.lastN          = N;
  bc.lastSampleRate = sampleRate;
}

static void step(_NT_algorithm *self, float *bus, int nfBy4) {
  auto *a = (_vocoderAlgorithm *)self;
  updateControlState(a);

  const int N = nfBy4 * 4;
  // The current disting NT callback ceiling is 24 frames. Keep the stack
  // buffers fixed and fail closed if a future host violates that contract.
  if (N <= 0 || N > 24) {
    return;
  }
  const float wet = vocoderClamp(a->controls.currentWet / 100.0f, 0.0f, 1.0f);
  const float preGainLinear =
      a->controls.currentOutputGainDb <= -59.9f
          ? 0.0f
          : powf(10.0f, a->controls.currentOutputGainDb / 20.0f);
  const int carrierBusL = self->v[kInCarrier];
  const bool carrierStereo = self->v[kCarrierStereo] > 0 &&
                             carrierBusL < kNT_lastBus;
  const int carrierBusR =
      carrierStereo ? carrierBusL + 1 : carrierBusL;
  const int modulatorBusL = self->v[kInModulator];
  const bool modulatorStereo = self->v[kModulatorStereo] > 0 &&
                               modulatorBusL < kNT_lastBus;
  const int modulatorBusR =
      modulatorStereo ? modulatorBusL + 1 : modulatorBusL;
  const int outputBusL = self->v[kOut];
  const bool stereoOutput = (carrierStereo || modulatorStereo) &&
                            outputBusL < kNT_lastBus;
  const int outputBusR = stereoOutput ? outputBusL + 1 : outputBusL;

  const float *carL = bus + (carrierBusL - 1) * N;
  const float *carR = bus + (carrierBusR - 1) * N;
  const float *modL = bus + (modulatorBusL - 1) * N;
  const float *modR = bus + (modulatorBusR - 1) * N;
  float *outL = bus + (outputBusL - 1) * N;
  float *outR = bus + (outputBusR - 1) * N;
  const bool replace = self->v[kOutMode] > 0;
  const bool bypass = self->vIncludingCommon && self->vIncludingCommon[0];
  const float sampleRate = (float)NT_globals.sampleRate;
  VocoderDSPState &s = *a->state;

  // Source availability can change without changing the number of output
  // channels (e.g. stereo carrier + mono modulator). Synchronise only the
  // state owned by the source that changed, preserving the still-stereo
  // source's independent right-channel history.
  if (carrierStereo != s.carrierStereoWasActive) {
    synchroniseRightCarrierState(s);
  }
  if (modulatorStereo != s.modulatorStereoWasActive) {
    synchroniseRightModulatorState(s);
  }

  // Channel 1 was wholly dormant during a preceding effective-mono interval.
  // Seed its remaining output-level state before it can render again,
  // including when the routing transition happens while bypassed.
  if (stereoOutput && !s.stereoOutputWasActive) {
    synchroniseRightChannelState(s);
  }
  s.carrierStereoWasActive = carrierStereo;
  s.modulatorStereoWasActive = modulatorStereo;
  s.stereoOutputWasActive = stereoOutput;

  if (bypass) {
    // Routing may overlap (for example Carrier 1/2 to Output 2/3), so both
    // source channels must be captured before either destination is written.
    float bypassCarrier[2][24];
    memcpy(bypassCarrier[0], carL, N * sizeof(float));
    if (stereoOutput) {
      memcpy(bypassCarrier[1], carR, N * sizeof(float));
    }
    for (int i = 0; i < N; ++i) {
      if (replace) {
        outL[i] = bypassCarrier[0][i];
      } else {
        outL[i] += bypassCarrier[0][i];
      }
      if (stereoOutput) {
        if (replace) {
          outR[i] = bypassCarrier[1][i];
        } else {
          outR[i] += bypassCarrier[1][i];
        }
      }
    }
    return;
  }

  // Recompute block coefficients only when inputs change (attack/release params,
  // activeBands, N, or sampleRate).  All other calls reuse the cached values,
  // eliminating ~24 expf/sqrtf calls per block.
  {
    VocoderCachedCoeffs &bc = a->blockCoeffs;
    if (bc.lastN != N || bc.lastSampleRate != sampleRate ||
        bc.lastAttack != self->v[kAttack] ||
        bc.lastRelease != self->v[kRelease] ||
        bc.lastActiveBands != a->activeBands) {
      bc.lastN          = N;
      bc.lastSampleRate = sampleRate;
      bc.lastAttack     = self->v[kAttack];
      bc.lastRelease    = self->v[kRelease];
      bc.lastActiveBands = a->activeBands;
      computeBlockCoeffs(a, N, sampleRate);
    }
  }
  const VocoderCachedCoeffs &bc = a->blockCoeffs;

  const float attackMix           = bc.attackMix;
  const float releaseMix          = bc.releaseMix;
  const float envAvgRiseMix       = bc.envAvgRiseMix;
  const float envAvgFallMix       = bc.envAvgFallMix;
  const float synthesisCoeffMix   = bc.synthesisCoeffMix;
  const float synthesisScalarMix  = bc.synthesisScalarMix;
  const float gainRiseMix         = bc.gainRiseMix;
  const float gainFallMix         = bc.gainFallMix;
  const float masterScale         = bc.masterScale;
  const float dcBlockR            = bc.dcBlockR;
  const float levelAvgRiseMix     = bc.levelAvgRiseMix;
  const float levelAvgFallMix     = bc.levelAvgFallMix;
  const float meterRiseMix        = bc.meterRiseMix;
  const float meterFallMix        = bc.meterFallMix;
  const float makeupRiseMix       = bc.makeupRiseMix;
  const float makeupFallMix       = bc.makeupFallMix;
  const float inputPeakRiseMix    = bc.inputPeakRiseMix;
  const float inputPeakFallMix    = bc.inputPeakFallMix;
  const float inputGuardAttackMix = bc.inputGuardAttackMix;
  const float inputGuardReleaseMix= bc.inputGuardReleaseMix;
  const float guardAttackMix      = bc.guardAttackMix;
  const float guardReleaseMix     = bc.guardReleaseMix;

  const int meterControlInterval = 128;
  const float inputGuardCeiling = 4.5f;
  const float guardCeiling = 5.5f;
  // Below -100 dBFS peak, the dry/wet ratio is no longer a useful level-match
  // measurement. Returning the automatic makeup gently to unity avoids storing
  // a 6x gain during silent modulator passages and exposing it on the next
  // transient.
  const float levelActivityFloor = 1.0e-5f;
  const VocoderDepthShape depthShape =
      computeDepthShape((float)self->v[kDepth]);
  VocoderDescriptor &d = *a->descriptor;
  const int channels = stereoOutput ? 2 : 1;

  // Per-block coefficient smoothing (was per-sample in old code —
  // once-per-block is sufficient and saves 40×N iterations)
  if (a->controls.synthesisCoeffSmoothing) {
    smoothSynthesisCoefficients(s, a->activeBands, synthesisCoeffMix);
  }

  // Smooth per-band scalars once per block
  {
    const float blockScalar = synthesisScalarMix;
    const float oneMinusScalar = 1.0f - blockScalar;
    for (int band = 0; band < a->activeBands; ++band) {
      s.synthesisBandGainCurrent[band] =
          blockScalar * s.synthesisBandGainCurrent[band] +
          oneMinusScalar * d.synthesisBandGain[band];
    }
    s.bandwidthCompCurrent = blockScalar * s.bandwidthCompCurrent +
                             oneMinusScalar * d.bandwidthCompensation;
  }

  // ── PHASE 1: Prepare input buffers with DC blocking + input guard ──
  // Use stack buffers for the block (N ≤ 24)
  float prepCarrier[2][24];
  float prepMod[2][24];

  for (int ch = 0; ch < channels; ++ch) {
    const float *rawCar = ch == 0 ? carL : carR;
    const float *rawMod = ch == 0 ? modL : modR;
    for (int i = 0; i < N; ++i) {
      const float drivenCar = rawCar[i] * preGainLinear;
      const float drivenMod = rawMod[i] * preGainLinear;
      const float dcCar = vocoderDcBlock(drivenCar, s.carrierDcX1[ch],
                                         s.carrierDcY1[ch], dcBlockR);
      const float dcMod =
          vocoderDcBlock(drivenMod, s.modDcX1[ch], s.modDcY1[ch], dcBlockR);
      // Input guard
      const float inputPeak =
          fabsf(dcCar) > fabsf(dcMod) ? fabsf(dcCar) : fabsf(dcMod);
      const float ipMix = inputPeak > s.inputPeakSmoothed[ch]
                              ? inputPeakRiseMix
                              : inputPeakFallMix;
      s.inputPeakSmoothed[ch] =
          ipMix * s.inputPeakSmoothed[ch] + (1.0f - ipMix) * inputPeak;
      const float targetIG = vocoderClamp(
          inputGuardCeiling / (s.inputPeakSmoothed[ch] + 1.0e-3f), 0.0f, 1.0f);
      const float igMix = targetIG < s.inputGuard[ch] ? inputGuardAttackMix
                                                      : inputGuardReleaseMix;
      s.inputGuard[ch] = igMix * s.inputGuard[ch] + (1.0f - igMix) * targetIG;
      if (s.inputGuard[ch] <= 0.0f) {
        s.inputGuard[ch] = 1.0f;
      }
      prepCarrier[ch][i] = dcCar * s.inputGuard[ch];
      prepMod[ch][i] = dcMod * s.inputGuard[ch];
    }
  }

  // ── PHASE 2: Makeup / guard ramps for the block ──
  float makeupRamp[2][24];
  float guardRamp[2][24];
  for (int ch = 0; ch < channels; ++ch) {
    // Targets are produced at the end of the preceding callback. Derive the
    // step from the block that will actually consume it so hosts that vary N
    // still complete a click-free ramp exactly on this block's final sample.
    s.wetMakeupStep[ch] =
        (s.wetMakeupTarget[ch] - s.wetMakeup[ch]) / (float)N;
    s.outputGuardStep[ch] =
        (s.outputGuardTarget[ch] - s.outputGuard[ch]) / (float)N;
    for (int i = 0; i < N; ++i) {
      float nextMakeup = s.wetMakeup[ch] + s.wetMakeupStep[ch];
      if ((s.wetMakeupStep[ch] >= 0.0f && nextMakeup > s.wetMakeupTarget[ch]) ||
          (s.wetMakeupStep[ch] < 0.0f && nextMakeup < s.wetMakeupTarget[ch])) {
        s.wetMakeup[ch] = s.wetMakeupTarget[ch];
        s.wetMakeupStep[ch] = 0.0f;
      } else {
        s.wetMakeup[ch] = nextMakeup;
      }
      makeupRamp[ch][i] = s.wetMakeup[ch];

      float nextGuard = s.outputGuard[ch] + s.outputGuardStep[ch];
      if ((s.outputGuardStep[ch] >= 0.0f &&
           nextGuard > s.outputGuardTarget[ch]) ||
          (s.outputGuardStep[ch] < 0.0f &&
           nextGuard < s.outputGuardTarget[ch])) {
        s.outputGuard[ch] = s.outputGuardTarget[ch];
        s.outputGuardStep[ch] = 0.0f;
      } else {
        s.outputGuard[ch] = nextGuard;
      }
      guardRamp[ch][i] = s.outputGuard[ch];
    }
    s.wetMakeup[ch] = s.wetMakeupTarget[ch];
    s.outputGuard[ch] = s.outputGuardTarget[ch];
    s.wetMakeupStep[ch] = 0.0f;
    s.outputGuardStep[ch] = 0.0f;
  }

  // ── PHASE 3: Band-first batch processing ──
  // Accumulate wet output per channel
  float wetAccum[2][24];
  memset(wetAccum, 0, sizeof(wetAccum));

  // Analysis output buffer (synthesis output is fused directly into wetAccum)
  float analysisBuf[24];

  for (int band = 0; band < a->activeBands; ++band) {
    float meterPeakBand = 0.0f;

    for (int ch = 0; ch < channels; ++ch) {
      // ── Analysis: batch biquad on modulator → analysisBuf ──
      const float analysisPeak = batchBiquadProcessWithEnvelope(
          s.anCoeffs[band], s.anState[ch][band], prepMod[ch], analysisBuf, N);

      // ── Envelope follower: update once per block ──
      // Meter tracks modulator (voice) energy per band.
      if (analysisPeak > meterPeakBand) {
        meterPeakBand = analysisPeak;
      }

      const float envInput = analysisPeak;
      const float envMix = envInput > s.env[ch][band] ? attackMix : releaseMix;
      s.env[ch][band] = (1.0f - envMix) * envInput + envMix * s.env[ch][band];

      const float envAmplitude = s.env[ch][band];
      const float envAvgMix =
          envAmplitude > s.eAvg[ch][band] ? envAvgRiseMix : envAvgFallMix;
      s.eAvg[ch][band] =
          envAvgMix * s.eAvg[ch][band] + (1.0f - envAvgMix) * envAmplitude;
      const float envNorm = envAmplitude / (s.eAvg[ch][band] + kVocoderEpsilon);
      const float rawBandGain = vocoderClamp(
          vocoderSoftKneeCompress(
              computeDepthGain(depthShape, envAmplitude, envNorm), 3.0f, 3.0f),
          0.0f, 8.0f);

      const float finalGainTarget =
          vocoderSoftKneeCompress(rawBandGain, 2.5f, 6.0f);
      // ── C: hoist gain direction once per band/channel, before the N-loop ──
      // gainTarget doesn't change mid-block and exponential smoothing never
      // overshoots, so the rise/fall branch is constant for this block.
      const float gainMix     = finalGainTarget > s.gainState[ch][band]
                                    ? gainRiseMix : gainFallMix;
      const float gainMixComp = 1.0f - gainMix;

      // ── B: fused synthesis biquad + gain smoothing + accumulation ──
      batchBiquadProcessAndAccum(s.syState[ch][band], prepCarrier[ch],
                                 wetAccum[ch], N, s.gainState[ch][band],
                                 finalGainTarget, gainMix, gainMixComp,
                                 s.synthesisBandGainCurrent[band]);
    }

    if (meterPeakBand > s.meterPeakHold[band]) {
      s.meterPeakHold[band] = meterPeakBand;
    }
  }

  // ── PHASE 4: Crossfade, output mixing, metering ──
  for (int i = 0; i < N; ++i) {
    const float shapedL = wetAccum[0][i] * masterScale * s.bandwidthCompCurrent;
    const float dryLevelL = fabsf(carL[i]);
    const float wetLevelL = fabsf(shapedL);
    if (dryLevelL > s.dryPeakHold[0]) {
      s.dryPeakHold[0] = dryLevelL;
    }
    if (wetLevelL > s.wetPeakHold[0]) {
      s.wetPeakHold[0] = wetLevelL;
    }
    const float matchedWetL = shapedL * makeupRamp[0][i];
    const float guardedWetL = matchedWetL * guardRamp[0][i];
    const float limitedWetL = vocoderTransparentLimit(guardedWetL, 9.0f);
    const float mixedL = carL[i] * (1.0f - wet) + limitedWetL * wet;

    if (replace) {
      outL[i] = mixedL;
    } else {
      outL[i] += mixedL;
    }

    if (stereoOutput) {
      const float shapedR =
          wetAccum[1][i] * masterScale * s.bandwidthCompCurrent;
      const float dryLevelR = fabsf(carR[i]);
      const float wetLevelR = fabsf(shapedR);
      if (dryLevelR > s.dryPeakHold[1]) {
        s.dryPeakHold[1] = dryLevelR;
      }
      if (wetLevelR > s.wetPeakHold[1]) {
        s.wetPeakHold[1] = wetLevelR;
      }
      const float matchedWetR = shapedR * makeupRamp[1][i];
      const float guardedWetR = matchedWetR * guardRamp[1][i];
      const float limitedWetR = vocoderTransparentLimit(guardedWetR, 9.0f);
      const float mixedR = carR[i] * (1.0f - wet) + limitedWetR * wet;
      if (replace) {
        outR[i] = mixedR;
      } else {
        outR[i] += mixedR;
      }
    }
  }

  // ── PHASE 5: Metering and level matching (at reduced rates) ──
  s.controlPhase += N;
  if (s.controlPhase >= meterControlInterval) {
    for (int band = 0; band < a->activeBands; ++band) {
      const float meterTarget =
          vocoderClamp(2.5f * s.meterPeakHold[band], 0.0f, 1.0f);
      const float meterMix =
          meterTarget > s.meters[band] ? meterRiseMix : meterFallMix;
      s.meters[band] =
          meterMix * s.meters[band] + (1.0f - meterMix) * meterTarget;
      s.meterPeakHold[band] = 0.0f;
    }
    for (int band = a->activeBands; band < kVocoderMaxBands; ++band) {
      s.meters[band] = meterFallMix * s.meters[band];
      s.meterPeakHold[band] = 0.0f;
    }
    s.controlPhase -= meterControlInterval;
  }

  // Level matching is deliberately block-rate: one peak measurement and one
  // one-pole update per callback, followed by an N-sample ramp next callback.
  for (int ch = 0; ch < channels; ++ch) {
    const float dryLevel = s.dryPeakHold[ch];
    const float wetLevel = s.wetPeakHold[ch];
    const float dryAvgMix =
        dryLevel > s.dryAvg[ch] ? levelAvgRiseMix : levelAvgFallMix;
    const float wetAvgMix =
        wetLevel > s.wetAvg[ch] ? levelAvgRiseMix : levelAvgFallMix;
    s.dryAvg[ch] = dryAvgMix * s.dryAvg[ch] + (1.0f - dryAvgMix) * dryLevel;
    s.wetAvg[ch] = wetAvgMix * s.wetAvg[ch] + (1.0f - wetAvgMix) * wetLevel;
    const bool wetIsActive = wetLevel > levelActivityFloor;
    const float targetMakeup =
        wetIsActive
            ? vocoderClamp(1.35f * (s.dryAvg[ch] + 0.01f) /
                               (s.wetAvg[ch] + 0.01f),
                           1.0f, 6.0f)
            : 1.0f;
    // Use the slower makeup time constant for the silence return so normal
    // gaps do not abruptly pull the level down. Active over-level conditions
    // still use the fast fall coefficient for protection.
    const float mMix = !wetIsActive
                           ? makeupRiseMix
                           : (targetMakeup < s.wetMakeupTarget[ch]
                                  ? makeupFallMix
                                  : makeupRiseMix);
    s.wetMakeupTarget[ch] =
        mMix * s.wetMakeupTarget[ch] + (1.0f - mMix) * targetMakeup;
    if (s.wetMakeupTarget[ch] < 1.0f) {
      s.wetMakeupTarget[ch] = 1.0f;
    }

    const float matchedWetPeak = wetLevel * s.wetMakeupTarget[ch];
    const float targetGuard =
        vocoderClamp(guardCeiling / (matchedWetPeak + 1.0e-3f), 0.0f, 1.0f);
    const float gMix = targetGuard < s.outputGuardTarget[ch]
                           ? guardAttackMix
                           : guardReleaseMix;
    s.outputGuardTarget[ch] =
        gMix * s.outputGuardTarget[ch] + (1.0f - gMix) * targetGuard;
    if (s.outputGuardTarget[ch] <= 0.0f) {
      s.outputGuardTarget[ch] = 1.0f;
    }

    s.dryPeakHold[ch] = 0.0f;
    s.wetPeakHold[ch] = 0.0f;
  }

  if (channels == 1) {
    // Channel 1 is inaudible in mono mode. Keep its automatic level state in
    // lockstep with channel 0 so re-enabling stereo cannot expose a stale 6x
    // makeup or attenuated guard from an earlier stereo passage.
    s.dryAvg[1] = s.dryAvg[0];
    s.wetAvg[1] = s.wetAvg[0];
    s.wetMakeup[1] = s.wetMakeup[0];
    s.wetMakeupTarget[1] = s.wetMakeupTarget[0];
    s.wetMakeupStep[1] = 0.0f;
    s.outputGuard[1] = s.outputGuard[0];
    s.outputGuardTarget[1] = s.outputGuardTarget[0];
    s.outputGuardStep[1] = 0.0f;
    s.dryPeakHold[1] = 0.0f;
    s.wetPeakHold[1] = 0.0f;
  }

  // Check if coefficient smoothing has converged
  if (a->controls.synthesisCoeffSmoothing) {
    float maxCoeffDelta = 0.0f;
    for (int band = 0; band < a->activeBands; ++band) {
      float delta = fabsf(s.sy_b0_smooth[band] - s.sy_b0_target[band]);
      float d2 = fabsf(s.sy_b2_smooth[band] - s.sy_b2_target[band]);
      if (d2 > delta)
        delta = d2;
      d2 = fabsf(s.sy_a1_smooth[band] - s.sy_a1_target[band]);
      if (d2 > delta)
        delta = d2;
      d2 = fabsf(s.sy_a2_smooth[band] - s.sy_a2_target[band]);
      if (d2 > delta)
        delta = d2;
      if (delta > maxCoeffDelta) {
        maxCoeffDelta = delta;
      }
    }

    if (maxCoeffDelta < 1.0e-4f) {
      syncSynthesisCoefficients(a);
      a->controls.synthesisCoeffSmoothing = false;
    }
  }
}

static void serialise(_NT_algorithm *self, _NT_jsonStream &stream) {
  auto *a = (_vocoderAlgorithm *)self;
  stream.addMemberName("pre_gain_tenths");
  stream.addNumber(a->uiOutputGainDisplay);
}

static bool deserialise(_NT_algorithm *self, _NT_jsonParse &parse) {
  auto *a = (_vocoderAlgorithm *)self;
  int numMembers = 0;
  if (!parse.numberOfObjectMembers(numMembers)) {
    return false;
  }

  for (int i = 0; i < numMembers; ++i) {
    if (parse.matchName("pre_gain_tenths")) {
      int gain = 0;
      if (!parse.number(gain)) {
        return false;
      }
      gain = gain < -600 ? -600 : (gain > 120 ? 120 : gain);
      a->uiOutputGainDisplay = gain;
      a->controls.currentOutputGainDb = gain * 0.1f;
      a->controls.targetOutputGainDb = gain * 0.1f;
    } else {
      if (!parse.skipMember()) {
        return false;
      }
    }
  }

  return true;
}

static const _NT_factory factory = {
    .guid = NT_MULTICHAR('V', 'O', 'C', '2'),
    .name = "Vocoder",
    .description = "Native C++ vocoder",
    .numSpecifications = 0,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
    .draw = draw,
    .tags = kNT_tagEffect,
    .hasCustomUi = hasCustomUi,
    .customUi = customUi,
    .setupUi = setupUi,
    .serialise = serialise,
    .deserialise = deserialise,
    .parameterString = parameterString,
};

extern "C" uintptr_t pluginEntry(_NT_selector sel, uint32_t d) {
  switch (sel) {
  case kNT_selector_version:
    return kNT_apiVersionCurrent;
  case kNT_selector_numFactories:
    return 1;
  case kNT_selector_factoryInfo:
    return (uintptr_t)((d == 0) ? &factory : nullptr);
  default:
    return 0;
  }
}
