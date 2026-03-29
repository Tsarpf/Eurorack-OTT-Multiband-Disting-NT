#include "vocoder_dsp.h"
#include "vocoder_parameters.h"
#include "vocoder_structs.h"
#include <distingnt/api.h>
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

static float computeDepthGain(float depthPct, float envNorm) {
  const float depthD = vocoderClamp(depthPct / 100.0f, 0.0f, 1.0f) * 2.0f;
  const float clampedEnv = vocoderClamp(envNorm, 0.25f, 4.0f);
  if (depthD <= 1.0f) {
    return (1.0f - depthD) + depthD * clampedEnv;
  }
  return powf(clampedEnv, 1.0f + 1.5f * (depthD - 1.0f));
}

static void clearDescriptor(VocoderDescriptor &descriptor) {
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.activeBands = 8;
  descriptor.bandwidthCompensation = 1.0f;
  descriptor.analysisQ = 1.0f;
  descriptor.synthesisQ = 1.0f;
}

static void clearState(VocoderDSPState &state) { memset(&state, 0, sizeof(state)); }

static void rebuildDescriptor(_vocoderAlgorithm *a) {
  VocoderDescriptor &d = *a->descriptor;
  clearDescriptor(d);

  const int bandCount =
      a->v[kBandCount] < 4 ? 4 : (a->v[kBandCount] > 40 ? 40 : a->v[kBandCount]);
  const float minFreq = (float)a->v[kMinFreq];
  const float maxFreq =
      (float)(a->v[kMaxFreq] > a->v[kMinFreq] ? a->v[kMaxFreq] : a->v[kMinFreq] + 20);
  const float formantRatio = powf(2.0f, a->controls.currentFormant / 120.0f);
  const float sampleRate = (float)NT_globals.sampleRate;
  const float bandwidthPct = a->controls.currentBandwidth;
  const float bandwidthNorm = bandwidthPct / 100.0f;

  d.activeBands = bandCount;
  d.analysisQ = powf(18.0f, 1.0f - bandwidthNorm) * powf(0.8f, bandwidthNorm);
  d.synthesisQ = d.analysisQ * 0.7f + 0.5f;
  if (d.synthesisQ < 3.0f) {
    d.synthesisQ = 3.0f;
  }
  d.bandwidthCompensation = computeBandwidthCompensation(bandCount, bandwidthPct);

  const float step =
      bandCount > 1 ? powf(maxFreq / minFreq, 1.0f / (float)(bandCount - 1)) : 1.0f;

  for (int i = 0; i < bandCount; ++i) {
    const float f = minFreq * powf(step, (float)i);
    d.analysisFreq[i] = f;
    d.synthesisFreq[i] = vocoderClamp(f * formantRatio, 20.0f, 20000.0f);
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

  a->controls.currentBandwidth = 50.0f;
  a->controls.targetBandwidth = 50.0f;
  a->controls.currentFormant = 0.0f;
  a->controls.targetFormant = 0.0f;
  a->controls.currentWet = 100.0f;
  a->controls.targetWet = 100.0f;
  a->controls.descriptorDirty = true;
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

  if (a->controls.descriptorDirty || movingBandwidth || movingFormant) {
    rebuildDescriptor(a);
    a->controls.descriptorDirty = movingBandwidth || movingFormant;
  }
}

static void step(_NT_algorithm *self, float *bus, int nfBy4) {
  auto *a = (_vocoderAlgorithm *)self;
  updateControlState(a);

  const int N = nfBy4 * 4;
  const float wet = vocoderClamp(a->controls.currentWet / 100.0f, 0.0f, 1.0f);
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
  const float gainRiseMix = vocoderMixCoeffFromSeconds(sampleRate, 0.0015f);
  const float gainFallMix = vocoderMixCoeffFromSeconds(sampleRate, 0.005f);
  const float masterScale = 1.4f / sqrtf((float)a->activeBands);
  VocoderDescriptor &d = *a->descriptor;
  VocoderDSPState &s = *a->state;

  for (int i = 0; i < N; ++i) {
    float wetSample[2] = {0.0f, 0.0f};
    float meterMax[kVocoderMaxBands] = {0.0f};

    const int channels = stereoOutput ? 2 : 1;
    for (int ch = 0; ch < channels; ++ch) {
      const float carrierSample = ch == 0 ? carL[i] : carR[i];
      const float modSample = ch == 0 ? modL[i] : modR[i];

      for (int band = 0; band < a->activeBands; ++band) {
        const float ya = d.an_b0[band] * modSample +
                         d.an_b2[band] * s.mod_x2[ch] -
                         d.an_a1[band] * s.an_y1[ch][band] -
                         d.an_a2[band] * s.an_y2[ch][band];
        s.an_y2[ch][band] = s.an_y1[ch][band];
        s.an_y1[ch][band] = ya;

        const float energy = ya * ya;
        const float envMix = energy > s.env[ch][band] ? attackMix : releaseMix;
        s.env[ch][band] =
            (1.0f - envMix) * energy + envMix * s.env[ch][band];

        const float envAmplitude = sqrtf(s.env[ch][band] + kVocoderEpsilon);
        const float envAvgMix =
            envAmplitude > s.eAvg[ch][band] ? envAvgRiseMix : envAvgFallMix;
        s.eAvg[ch][band] = envAvgMix * s.eAvg[ch][band] +
                           (1.0f - envAvgMix) * envAmplitude;
        const float envNorm =
            envAmplitude / (s.eAvg[ch][band] + kVocoderEpsilon);
        const float rawBandGain = vocoderClamp(
            computeDepthGain((float)self->v[kDepth], envNorm), 0.0f, 8.0f);

        const float ys = d.sy_b0[band] * carrierSample +
                         d.sy_b2[band] * s.car_x2[ch] -
                         d.sy_a1[band] * s.sy_y1[ch][band] -
                         d.sy_a2[band] * s.sy_y2[ch][band];
        s.sy_y2[ch][band] = s.sy_y1[ch][band];
        s.sy_y1[ch][band] = ys;

        float enhanceGain = 1.0f;
        if (enhance) {
          const float carrierEnergy = ys * ys;
          const float carrierAvgMix =
              carrierEnergy > s.cAvg[ch][band] ? carrierAvgRiseMix
                                               : carrierAvgFallMix;
          s.cAvg[ch][band] = carrierAvgMix * s.cAvg[ch][band] +
                             (1.0f - carrierAvgMix) * carrierEnergy;
          if (s.cAvg[ch][band] > 1.0e-5f) {
            enhanceGain = vocoderClamp(
                d.enhanceTarget[band] /
                    (sqrtf(s.cAvg[ch][band]) + kVocoderEpsilon),
                0.67f, 1.5f);
          }
        }

        const float targetGain = rawBandGain * enhanceGain;
        const float gainMix =
            targetGain > s.gainState[ch][band] ? gainRiseMix : gainFallMix;
        s.gainState[ch][band] =
            gainMix * s.gainState[ch][band] + (1.0f - gainMix) * targetGain;

        wetSample[ch] += ys * s.gainState[ch][band];
        if (s.gainState[ch][band] > meterMax[band]) {
          meterMax[band] = s.gainState[ch][band];
        }
      }

      s.mod_x2[ch] = s.mod_x1[ch];
      s.mod_x1[ch] = modSample;
      s.car_x2[ch] = s.car_x1[ch];
      s.car_x1[ch] = carrierSample;
    }

    for (int band = a->activeBands; band < kVocoderMaxBands; ++band) {
      s.meters[band] = vocoderSmoothToward(s.meters[band], 0.0f, 0.18f);
    }

    for (int band = 0; band < a->activeBands; ++band) {
      s.meters[band] = vocoderSmoothToward(
          s.meters[band], vocoderClamp(0.25f * meterMax[band], 0.0f, 1.0f),
          0.18f);
    }

    const float shapedL = wetSample[0] * masterScale * d.bandwidthCompensation;
    const float limitedWetL = vocoderSoftSaturate(shapedL);
    const float mixedL = carL[i] * (1.0f - wet) + limitedWetL * wet;

    if (replace) {
      outL[i] = mixedL;
    } else {
      outL[i] += mixedL;
    }

    if (stereoOutput) {
      const float shapedR =
          wetSample[1] * masterScale * d.bandwidthCompensation;
      const float limitedWetR = vocoderSoftSaturate(shapedR);
      const float mixedR = carR[i] * (1.0f - wet) + limitedWetR * wet;
      if (replace) {
        outR[i] = mixedR;
      } else {
        outR[i] += mixedR;
      }
    }
  }
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
