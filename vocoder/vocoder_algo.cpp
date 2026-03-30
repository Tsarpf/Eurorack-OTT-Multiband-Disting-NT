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

static float computeDepthGain(float depthPct, float envAmplitude,
                              float envNorm) {
  const float depthControl = vocoderClamp(depthPct / 100.0f, 0.0f, 2.0f);
  const float depthMix = vocoderClamp(depthControl, 0.0f, 1.0f);
  const float presence = vocoderClamp(envAmplitude * 5.0f, 0.0f, 1.0f);
  const float clampedEnv = vocoderClamp(envNorm, 0.25f, 4.0f);
  const float classicShape = presence * clampedEnv;
  if (depthControl <= 1.0f) {
    return vocoderLerp(1.0f, classicShape, depthMix);
  }

  const float peakShape =
      presence * powf(clampedEnv, 1.0f + 1.5f * (depthControl - 1.0f));
  return peakShape;
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
  const float x2 = x * x;
  if (shape.peakExponent <= 2.0f) {
    return presence * vocoderLerp(x, x2, shape.peakExponent - 1.0f);
  }
  const float x3 = x2 * x;
  if (shape.peakExponent <= 3.0f) {
    return presence * vocoderLerp(x2, x3, shape.peakExponent - 2.0f);
  }
  const float x4 = x3 * x;
  if (shape.peakExponent <= 4.0f) {
    return presence * vocoderLerp(x3, x4, shape.peakExponent - 3.0f);
  }
  const float x5 = x4 * x;
  return presence * vocoderLerp(x4, x5, shape.peakExponent - 4.0f);
}

static void clearDescriptor(VocoderDescriptor &descriptor) {
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.activeBands = 8;
  descriptor.bandwidthCompensation = 1.0f;
  descriptor.analysisQ = 1.0f;
  descriptor.synthesisQ = 1.0f;
}

static void clearState(VocoderDSPState &state) { memset(&state, 0, sizeof(state)); }

static void beginSynthesisCrossfade(_vocoderAlgorithm *a) {
  VocoderDSPState &s = *a->state;
  s.prevActiveBands = a->activeBands;
  for (int ch = 0; ch < 2; ++ch) {
    for (int band = 0; band < s.prevActiveBands; ++band) {
      s.prev_sy_y1[ch][band] = s.sy_y1[ch][band];
      s.prev_sy_y2[ch][band] = s.sy_y2[ch][band];
    }
  }
  for (int band = 0; band < s.prevActiveBands; ++band) {
    s.prev_sy_b0[band] = s.sy_b0_current[band];
    s.prev_sy_b2[band] = s.sy_b2_current[band];
    s.prev_sy_a1[band] = s.sy_a1_current[band];
    s.prev_sy_a2[band] = s.sy_a2_current[band];
  }
  s.synthesisXfadeTotal = NT_globals.sampleRate / 1000;
  if (s.synthesisXfadeTotal < 1) {
    s.synthesisXfadeTotal = 1;
  }
  s.synthesisXfadeRemaining = s.synthesisXfadeTotal;
}

static void syncSynthesisCoefficients(_vocoderAlgorithm *a) {
  VocoderDescriptor &d = *a->descriptor;
  VocoderDSPState &s = *a->state;
  for (int band = 0; band < kVocoderMaxBands; ++band) {
    s.sy_b0_current[band] = d.sy_b0[band];
    s.sy_b2_current[band] = d.sy_b2[band];
    s.sy_a1_current[band] = d.sy_a1[band];
    s.sy_a2_current[band] = d.sy_a2[band];
  }
}

static void rebuildDescriptor(_vocoderAlgorithm *a) {
  VocoderDescriptor &d = *a->descriptor;
  clearDescriptor(d);

  const int bandCount =
      a->v[kBandCount] < 4 ? 4 : (a->v[kBandCount] > 40 ? 40 : a->v[kBandCount]);
  const float minFreq = (float)(a->v[kMinFreq] < 30 ? 30 : a->v[kMinFreq]);
  const float maxFreq =
      (float)(a->v[kMaxFreq] > minFreq ? a->v[kMaxFreq] : minFreq + 20);
  const float formantRatio = powf(2.0f, a->controls.currentFormant / 120.0f);
  const float sampleRate = (float)NT_globals.sampleRate;
  const float bandwidthPct = a->controls.currentBandwidth;
  const float bandwidthNorm = bandwidthPct / 100.0f;
  const float bandwidthCurve = bandwidthNorm * bandwidthNorm;
  const float synthesisFloorHz = 30.0f;
  const float synthesisFadeStartHz = 20.0f;

  d.activeBands = bandCount;
  d.analysisQ = powf(40.0f, 1.0f - bandwidthCurve) * powf(0.7f, bandwidthCurve);
  d.synthesisQ = d.analysisQ * 0.85f + 1.0f;
  if (d.synthesisQ < 3.0f) {
    d.synthesisQ = 3.0f;
  }
  d.bandwidthCompensation = computeBandwidthCompensation(bandCount, bandwidthPct);

  const float step =
      bandCount > 1 ? powf(maxFreq / minFreq, 1.0f / (float)(bandCount - 1)) : 1.0f;

  for (int i = 0; i < bandCount; ++i) {
    const float f = minFreq * powf(step, (float)i);
    const float shiftedFreq = f * formantRatio;
    d.analysisFreq[i] = f;
    d.synthesisFreq[i] = vocoderClamp(shiftedFreq, synthesisFloorHz, 20000.0f);
    d.synthesisBandGain[i] = vocoderClamp(
        (shiftedFreq - synthesisFadeStartHz) /
            (synthesisFloorHz - synthesisFadeStartHz),
        0.0f, 1.0f);
    d.enhanceTarget[i] =
        0.12f * powf(f / (minFreq > 1.0f ? minFreq : 1.0f), 0.1f);

    vocoderCalculateBandpass(f, d.analysisQ, sampleRate, d.an_b0[i], d.an_b2[i],
                             d.an_a1[i], d.an_a2[i]);
    vocoderCalculateBandpass(d.synthesisFreq[i], d.synthesisQ, sampleRate,
                             d.sy_b0[i], d.sy_b2[i], d.sy_a1[i], d.sy_a2[i]);
  }

  a->activeBands = bandCount;
  a->uiDirty = true;
}

static void calculateRequirements(_NT_algorithmRequirements &req, const int32_t *) {
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
  a->state->inputGuard[0] = 1.0f;
  a->state->inputGuard[1] = 1.0f;
  a->state->outputGuard[0] = 1.0f;
  a->state->outputGuard[1] = 1.0f;

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
    a->controls.descriptorDirty = true;
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
  case kEnhance:
    a->controls.descriptorDirty = true;
    break;
  default:
    break;
  }
  a->uiDirty = true;
}

static void updateControlState(_vocoderAlgorithm *a) {
  constexpr float kControlSmoothing = 0.35f;
  const bool movingBandwidth =
      fabsf(a->controls.targetBandwidth - a->controls.currentBandwidth) > 0.001f;
  const bool movingFormant =
      fabsf(a->controls.targetFormant - a->controls.currentFormant) > 0.001f;

  a->controls.currentBandwidth = vocoderSmoothToward(
      a->controls.currentBandwidth, a->controls.targetBandwidth,
      kControlSmoothing);
  a->controls.currentFormant = vocoderSmoothToward(
      a->controls.currentFormant, a->controls.targetFormant,
      kControlSmoothing);
  a->controls.currentWet =
      vocoderSmoothToward(a->controls.currentWet, a->controls.targetWet,
                          kControlSmoothing);
  a->controls.currentOutputGainDb =
      vocoderSmoothToward(a->controls.currentOutputGainDb,
                          a->controls.targetOutputGainDb, kControlSmoothing);

  if (a->controls.descriptorDirty || movingBandwidth || movingFormant) {
    if (movingBandwidth || movingFormant) {
      beginSynthesisCrossfade(a);
    }
    rebuildDescriptor(a);
    if (movingBandwidth || movingFormant) {
      a->controls.synthesisCoeffSmoothing = true;
    } else {
      syncSynthesisCoefficients(a);
      a->controls.synthesisCoeffSmoothing = false;
    }
    a->controls.descriptorDirty = movingBandwidth || movingFormant;
  }
}

static void step(_NT_algorithm *self, float *bus, int nfBy4) {
  auto *a = (_vocoderAlgorithm *)self;
  updateControlState(a);

  const int N = nfBy4 * 4;
  const float wet = vocoderClamp(a->controls.currentWet / 100.0f, 0.0f, 1.0f);
  const float preGainLinear =
      a->uiOutputGainDisplay <= -600
          ? 0.0f
          : powf(10.0f, a->controls.currentOutputGainDb / 20.0f);
  const bool carrierStereo = self->v[kCarrierStereo] > 0;
  const bool modulatorStereo = self->v[kModulatorStereo] > 0;
  const bool stereoOutput = carrierStereo || modulatorStereo;

  const int carrierBusL = self->v[kInCarrier];
  const int carrierBusR = carrierStereo && carrierBusL < 28 ? carrierBusL + 1
                                                            : carrierBusL;
  const int modulatorBusL = self->v[kInModulator];
  const int modulatorBusR =
      modulatorStereo && modulatorBusL < 28 ? modulatorBusL + 1 : modulatorBusL;
  const int outputBusL = self->v[kOut];
  const int outputBusR = stereoOutput && outputBusL < 28 ? outputBusL + 1
                                                         : outputBusL;

  const float *carL = bus + (carrierBusL - 1) * N;
  const float *carR = bus + (carrierBusR - 1) * N;
  const float *modL = bus + (modulatorBusL - 1) * N;
  const float *modR = bus + (modulatorBusR - 1) * N;
  float *outL = bus + (outputBusL - 1) * N;
  float *outR = bus + (outputBusR - 1) * N;
  const bool replace = self->v[kOutMode] > 0;
  const bool enhance = self->v[kEnhance] > 0;
  const bool bypass = self->vIncludingCommon && self->vIncludingCommon[0];
  const float sampleRate = (float)NT_globals.sampleRate;

  if (bypass) {
    for (int i = 0; i < N; ++i) {
      if (replace) {
        outL[i] = carL[i];
      } else {
        outL[i] += carL[i];
      }
      if (stereoOutput) {
        if (replace) {
          outR[i] = carR[i];
        } else {
          outR[i] += carR[i];
        }
      }
    }
    return;
  }

  const float attackMix =
      vocoderMixCoeffFromSeconds(sampleRate, (float)self->v[kAttack] * 0.001f);
  const float releaseMix =
      vocoderMixCoeffFromSeconds(sampleRate, (float)self->v[kRelease] * 0.001f);
  // Track normalization baselines quickly on rising energy to avoid transient
  // overboost, but decay them more slowly so the vocoder still breathes
  // naturally between hits.
  const float envAvgRiseMix = vocoderMixCoeffFromSeconds(sampleRate, 0.015f);
  const float envAvgFallMix = vocoderMixCoeffFromSeconds(sampleRate, 0.05f);
  const float carrierAvgRiseMix =
      vocoderMixCoeffFromSeconds(sampleRate, 0.01f);
  const float carrierAvgFallMix =
      vocoderMixCoeffFromSeconds(sampleRate, 0.08f);
  const float synthesisCoeffMix =
      vocoderMixCoeffFromSeconds(sampleRate, 0.0015f);
  const float gainRiseMix = vocoderMixCoeffFromSeconds(sampleRate, 0.0015f);
  const float gainFallMix = vocoderMixCoeffFromSeconds(sampleRate, 0.005f);
  const int bandControlInterval = 4;
  const int controlInterval = 128;
  const float controlSampleRate = sampleRate / (float)controlInterval;
  const float masterScale = 8.0f / sqrtf((float)a->activeBands);
  const float dcBlockR = expf(-2.0f * 3.14159265359f * 30.0f / sampleRate);
  const float levelAvgRiseMix =
      vocoderMixCoeffFromSeconds(controlSampleRate, 0.01f);
  const float levelAvgFallMix =
      vocoderMixCoeffFromSeconds(controlSampleRate, 0.12f);
  const float meterRiseMix =
      vocoderMixCoeffFromSeconds(controlSampleRate, 0.01f);
  const float meterFallMix =
      vocoderMixCoeffFromSeconds(controlSampleRate, 0.08f);
  const float makeupRiseMix =
      vocoderMixCoeffFromSeconds(controlSampleRate, 0.05f);
  const float makeupFallMix =
      vocoderMixCoeffFromSeconds(controlSampleRate, 0.008f);
  const float inputGuardAttackMix =
      vocoderMixCoeffFromSeconds(sampleRate, 0.00035f);
  const float inputGuardReleaseMix =
      vocoderMixCoeffFromSeconds(sampleRate, 0.04f);
  const float inputGuardCeiling = 4.5f;
  const float guardAttackMix =
      vocoderMixCoeffFromSeconds(controlSampleRate, 0.0005f);
  const float guardReleaseMix =
      vocoderMixCoeffFromSeconds(controlSampleRate, 0.05f);
  const float guardCeiling = 5.5f;
  const VocoderDepthShape depthShape =
      computeDepthShape((float)self->v[kDepth]);
  VocoderDescriptor &d = *a->descriptor;
  VocoderDSPState &s = *a->state;
  float *sy_b0 = a->controls.synthesisCoeffSmoothing ? s.sy_b0_current : d.sy_b0;
  float *sy_b2 = a->controls.synthesisCoeffSmoothing ? s.sy_b2_current : d.sy_b2;
  float *sy_a1 = a->controls.synthesisCoeffSmoothing ? s.sy_a1_current : d.sy_a1;
  float *sy_a2 = a->controls.synthesisCoeffSmoothing ? s.sy_a2_current : d.sy_a2;

  for (int i = 0; i < N; ++i) {
    float wetSample[2] = {0.0f, 0.0f};
    float wetSamplePrev[2] = {0.0f, 0.0f};
    float carrierSample[2] = {0.0f, 0.0f};
    float modSample[2] = {0.0f, 0.0f};
    const bool updateBandControl = (s.bandControlPhase == 0);
    const bool flushControl = (s.controlPhase + 1 >= controlInterval);

    const int channels = stereoOutput ? 2 : 1;
    for (int ch = 0; ch < channels; ++ch) {
      const float rawCarrierSample = ch == 0 ? carL[i] : carR[i];
      const float rawModSample = ch == 0 ? modL[i] : modR[i];
      const float drivenCarrierSample = rawCarrierSample * preGainLinear;
      const float drivenModSample = rawModSample * preGainLinear;
      const float dcBlockedCarrier =
          vocoderDcBlock(drivenCarrierSample, s.carrierDcX1[ch],
                         s.carrierDcY1[ch], dcBlockR);
      const float dcBlockedMod =
          vocoderDcBlock(drivenModSample, s.modDcX1[ch], s.modDcY1[ch],
                         dcBlockR);
      const float inputPeak = fabsf(dcBlockedCarrier) > fabsf(dcBlockedMod)
                                  ? fabsf(dcBlockedCarrier)
                                  : fabsf(dcBlockedMod);
      const float targetInputGuard = vocoderClamp(
          inputGuardCeiling / (inputPeak + 1.0e-3f), 0.0f, 1.0f);
      const float inputGuardMix =
          targetInputGuard < s.inputGuard[ch] ? inputGuardAttackMix
                                              : inputGuardReleaseMix;
      s.inputGuard[ch] =
          inputGuardMix * s.inputGuard[ch] +
          (1.0f - inputGuardMix) * targetInputGuard;
      if (s.inputGuard[ch] <= 0.0f) {
        s.inputGuard[ch] = 1.0f;
      }
      carrierSample[ch] = dcBlockedCarrier * s.inputGuard[ch];
      modSample[ch] = dcBlockedMod * s.inputGuard[ch];
    }

    for (int band = 0; band < a->activeBands; ++band) {
      float meterPeak = 0.0f;
      for (int ch = 0; ch < channels; ++ch) {
        const float ya = d.an_b0[band] * modSample[ch] +
                         d.an_b2[band] * s.mod_x2[ch] -
                         d.an_a1[band] * s.an_y1[ch][band] -
                         d.an_a2[band] * s.an_y2[ch][band];
        s.an_y2[ch][band] = s.an_y1[ch][band];
        s.an_y1[ch][band] = ya;

        const float envPeak = fabsf(ya);
        if (envPeak > s.envPeakHold[ch][band]) {
          s.envPeakHold[ch][band] = envPeak;
        }

        if (a->controls.synthesisCoeffSmoothing) {
          sy_b0[band] =
              synthesisCoeffMix * sy_b0[band] + (1.0f - synthesisCoeffMix) * d.sy_b0[band];
          sy_b2[band] =
              synthesisCoeffMix * sy_b2[band] + (1.0f - synthesisCoeffMix) * d.sy_b2[band];
          sy_a1[band] =
              synthesisCoeffMix * sy_a1[band] + (1.0f - synthesisCoeffMix) * d.sy_a1[band];
          sy_a2[band] =
              synthesisCoeffMix * sy_a2[band] + (1.0f - synthesisCoeffMix) * d.sy_a2[band];
        }

        const float ys = sy_b0[band] * carrierSample[ch] +
                         sy_b2[band] * s.car_x2[ch] -
                         sy_a1[band] * s.sy_y1[ch][band] -
                         sy_a2[band] * s.sy_y2[ch][band];
        s.sy_y2[ch][band] = s.sy_y1[ch][band];
        s.sy_y1[ch][band] = ys;

        const float carrierPeak = fabsf(ys);
        if (carrierPeak > s.carrierPeakHold[ch][band]) {
          s.carrierPeakHold[ch][band] = carrierPeak;
        }

        if (updateBandControl) {
          const float envInput = s.envPeakHold[ch][band];
          const float envMix =
              envInput > s.env[ch][band] ? attackMix : releaseMix;
          s.env[ch][band] =
              (1.0f - envMix) * envInput + envMix * s.env[ch][band];

          const float envAmplitude = s.env[ch][band];
          const float envAvgMix =
              envAmplitude > s.eAvg[ch][band] ? envAvgRiseMix : envAvgFallMix;
          s.eAvg[ch][band] = envAvgMix * s.eAvg[ch][band] +
                             (1.0f - envAvgMix) * envAmplitude;
          const float envNorm =
              envAmplitude / (s.eAvg[ch][band] + kVocoderEpsilon);
          const float rawBandGain = vocoderClamp(
              vocoderSoftKneeCompress(
                  computeDepthGain(depthShape, envAmplitude, envNorm), 3.0f,
                  3.0f),
              0.0f, 8.0f);

          float enhanceGain = 1.0f;
          if (enhance) {
            const float carrierAmplitude = s.carrierPeakHold[ch][band];
            const float carrierAvgMix =
                carrierAmplitude > s.cAvg[ch][band] ? carrierAvgRiseMix
                                                    : carrierAvgFallMix;
            s.cAvg[ch][band] = carrierAvgMix * s.cAvg[ch][band] +
                               (1.0f - carrierAvgMix) * carrierAmplitude;
            if (s.cAvg[ch][band] > 1.0e-5f) {
              enhanceGain = vocoderClamp(
                  d.enhanceTarget[band] / (s.cAvg[ch][band] + kVocoderEpsilon),
                  0.67f, 1.5f);
            }
          }

          s.gainTarget[ch][band] =
              vocoderSoftKneeCompress(rawBandGain * enhanceGain, 2.5f, 6.0f);
          s.envPeakHold[ch][band] = 0.0f;
          s.carrierPeakHold[ch][band] = 0.0f;
        }
        const float gainMix =
            s.gainTarget[ch][band] > s.gainState[ch][band] ? gainRiseMix
                                                           : gainFallMix;
        s.gainState[ch][band] =
            gainMix * s.gainState[ch][band] +
            (1.0f - gainMix) * s.gainTarget[ch][band];

        const float bandWet =
            ys * s.gainState[ch][band] * d.synthesisBandGain[band];
        wetSample[ch] += bandWet;
        if (s.synthesisXfadeRemaining > 0 && band < s.prevActiveBands) {
          const float prevYs = s.prev_sy_b0[band] * carrierSample[ch] +
                               s.prev_sy_b2[band] * s.car_x2[ch] -
                               s.prev_sy_a1[band] * s.prev_sy_y1[ch][band] -
                               s.prev_sy_a2[band] * s.prev_sy_y2[ch][band];
          s.prev_sy_y2[ch][band] = s.prev_sy_y1[ch][band];
          s.prev_sy_y1[ch][band] = prevYs;
          wetSamplePrev[ch] +=
              prevYs * s.gainState[ch][band] * d.synthesisBandGain[band];
        }
        const float bandLevel = fabsf(bandWet);
        if (bandLevel > meterPeak) {
          meterPeak = bandLevel;
        }
      }

      if (meterPeak > s.meterPeakHold[band]) {
        s.meterPeakHold[band] = meterPeak;
      }
    }

    for (int ch = 0; ch < channels; ++ch) {
      s.mod_x2[ch] = s.mod_x1[ch];
      s.mod_x1[ch] = modSample[ch];
      s.car_x2[ch] = s.car_x1[ch];
      s.car_x1[ch] = carrierSample[ch];
    }

    if (s.synthesisXfadeRemaining > 0) {
      const float xfade = 1.0f -
                          (float)s.synthesisXfadeRemaining /
                              (float)s.synthesisXfadeTotal;
      const float mix = vocoderClamp(xfade, 0.0f, 1.0f);
      for (int ch = 0; ch < channels; ++ch) {
        wetSample[ch] = vocoderLerp(wetSamplePrev[ch], wetSample[ch], mix);
      }
      --s.synthesisXfadeRemaining;
    }

    const float shapedL = wetSample[0] * masterScale * d.bandwidthCompensation;
    const float dryLevelL = fabsf(carL[i]);
    const float wetLevelL = fabsf(shapedL);
    if (dryLevelL > s.dryPeakHold[0]) {
      s.dryPeakHold[0] = dryLevelL;
    }
    if (wetLevelL > s.wetPeakHold[0]) {
      s.wetPeakHold[0] = wetLevelL;
    }
    const float matchedWetL = shapedL * s.wetMakeup[0];
    const float guardedWetL = matchedWetL * s.outputGuard[0];
    const float limitedWetL = vocoderTransparentLimit(guardedWetL, 9.0f);
    const float mixedL = carL[i] * (1.0f - wet) + limitedWetL * wet;

    if (replace) {
      outL[i] = mixedL;
    } else {
      outL[i] += mixedL;
    }

    if (stereoOutput) {
      const float shapedR =
          wetSample[1] * masterScale * d.bandwidthCompensation;
      const float dryLevelR = fabsf(carR[i]);
      const float wetLevelR = fabsf(shapedR);
      if (dryLevelR > s.dryPeakHold[1]) {
        s.dryPeakHold[1] = dryLevelR;
      }
      if (wetLevelR > s.wetPeakHold[1]) {
        s.wetPeakHold[1] = wetLevelR;
      }
      const float matchedWetR = shapedR * s.wetMakeup[1];
      const float guardedWetR = matchedWetR * s.outputGuard[1];
      const float limitedWetR = vocoderTransparentLimit(guardedWetR, 9.0f);
      const float mixedR = carR[i] * (1.0f - wet) + limitedWetR * wet;
      if (replace) {
        outR[i] = mixedR;
      } else {
        outR[i] += mixedR;
      }
    }

    if (flushControl) {
      for (int band = 0; band < a->activeBands; ++band) {
        const float meterTarget = vocoderClamp(2.5f * s.meterPeakHold[band], 0.0f, 1.0f);
        const float meterMix =
            meterTarget > s.meters[band] ? meterRiseMix : meterFallMix;
        s.meters[band] =
            meterMix * s.meters[band] + (1.0f - meterMix) * meterTarget;
        s.meterPeakHold[band] = 0.0f;
      }
      for (int band = a->activeBands; band < kVocoderMaxBands; ++band) {
        s.meters[band] =
            meterFallMix * s.meters[band] + (1.0f - meterFallMix) * 0.0f;
        s.meterPeakHold[band] = 0.0f;
      }

      for (int ch = 0; ch < channels; ++ch) {
        const float dryLevel = s.dryPeakHold[ch];
        const float wetLevel = s.wetPeakHold[ch];
        const float dryAvgMix =
            dryLevel > s.dryAvg[ch] ? levelAvgRiseMix : levelAvgFallMix;
        const float wetAvgMix =
            wetLevel > s.wetAvg[ch] ? levelAvgRiseMix : levelAvgFallMix;
        s.dryAvg[ch] =
            dryAvgMix * s.dryAvg[ch] + (1.0f - dryAvgMix) * dryLevel;
        s.wetAvg[ch] =
            wetAvgMix * s.wetAvg[ch] + (1.0f - wetAvgMix) * wetLevel;
        const float targetMakeup = vocoderClamp(
            1.35f * (s.dryAvg[ch] + 0.01f) / (s.wetAvg[ch] + 0.01f), 1.0f, 6.0f);
        const float makeupMix =
            targetMakeup < s.wetMakeup[ch] ? makeupFallMix : makeupRiseMix;
        s.wetMakeup[ch] =
            makeupMix * s.wetMakeup[ch] + (1.0f - makeupMix) * targetMakeup;
        if (s.wetMakeup[ch] < 1.0f) {
          s.wetMakeup[ch] = 1.0f;
        }

        const float matchedWetPeak = wetLevel * s.wetMakeup[ch];
        const float targetGuard =
            vocoderClamp(guardCeiling / (matchedWetPeak + 1.0e-3f), 0.0f, 1.0f);
        const float guardMix =
            targetGuard < s.outputGuard[ch] ? guardAttackMix : guardReleaseMix;
        s.outputGuard[ch] =
            guardMix * s.outputGuard[ch] + (1.0f - guardMix) * targetGuard;
        if (s.outputGuard[ch] <= 0.0f) {
          s.outputGuard[ch] = 1.0f;
        }

        s.dryPeakHold[ch] = 0.0f;
        s.wetPeakHold[ch] = 0.0f;
      }

      s.controlPhase = 0;
    } else {
      ++s.controlPhase;
    }

    ++s.bandControlPhase;
    if (s.bandControlPhase >= bandControlInterval) {
      s.bandControlPhase = 0;
    }
  }

  if (a->controls.synthesisCoeffSmoothing) {
    float maxCoeffDelta = 0.0f;
    for (int band = 0; band < a->activeBands; ++band) {
      float delta = fabsf(s.sy_b0_current[band] - d.sy_b0[band]);
      if (fabsf(s.sy_b2_current[band] - d.sy_b2[band]) > delta) {
        delta = fabsf(s.sy_b2_current[band] - d.sy_b2[band]);
      }
      if (fabsf(s.sy_a1_current[band] - d.sy_a1[band]) > delta) {
        delta = fabsf(s.sy_a1_current[band] - d.sy_a1[band]);
      }
      if (fabsf(s.sy_a2_current[band] - d.sy_a2[band]) > delta) {
        delta = fabsf(s.sy_a2_current[band] - d.sy_a2[band]);
      }
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
