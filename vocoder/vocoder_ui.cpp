#include "vocoder_parameters.h"
#include "vocoder_structs.h"
#include <distingnt/api.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int mapMeterToHeight(float value) {
  const float clamped = vocoderClamp(value, 0.0f, 1.0f);
  return (int)(clamped * 30.0f);
}

static int roundTenths(float value) {
  return (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static void formatOutputGain(char *buf, size_t bufSize, int valueTenthsDb) {
  if (valueTenthsDb <= -600) {
    snprintf(buf, bufSize, "-inf");
  } else if ((valueTenthsDb % 10) == 0) {
    snprintf(buf, bufSize, "%+d", valueTenthsDb / 10);
  } else {
    const char sign = valueTenthsDb < 0 ? '-' : '+';
    const int absTenths = valueTenthsDb < 0 ? -valueTenthsDb : valueTenthsDb;
    snprintf(buf, bufSize, "%c%d.%1d", sign, absTenths / 10, absTenths % 10);
  }
}

static void formatDecay(char *buf, size_t bufSize, int valueMs) {
  if (valueMs < 1000) {
    snprintf(buf, bufSize, "%dms", valueMs);
  } else {
    const int tenths = roundTenths((float)valueMs / 100.0f);
    snprintf(buf, bufSize, "%d.%1ds", tenths / 10, tenths % 10);
  }
}

static int decayStepMs(int currentMs) {
  if (currentMs < 1000) {
    return 25;
  }
  if (currentMs < 5000) {
    return 100;
  }
  return 500;
}

void setupUi(_NT_algorithm *self, _NT_float3 &pots) {
  auto *a = (_vocoderAlgorithm *)self;
  pots[0] = vocoderClamp((float)a->v[kBandWidth] / 100.0f, 0.0f, 1.0f);
  pots[1] = vocoderClamp((float)a->v[kDepth] / 200.0f, 0.0f, 1.0f);
  pots[2] =
      vocoderClamp(((float)a->v[kFormant] + 240.0f) / 480.0f, 0.0f, 1.0f);
  a->uiReleaseDisplay = a->v[kRelease];
  a->uiWetDisplay = a->v[kWet];
  a->uiOutputGainDisplay = a->v[kPreGain];
}

bool draw(_NT_algorithm *self) {
  auto *a = (_vocoderAlgorithm *)self;
  memset(NT_screen, 0, sizeof(NT_screen));

  const int labelY = 6;
  const int valueY = 15;
  const int meterTop = 21;
  const int meterBottom = 53;
  const int footerY = 60;
  const int leftX = 43;
  const int centreX = 128;
  const int rightX = 213;

  char buf[32];
  NT_drawText(leftX, labelY, "BW", 15, kNT_textCentre, kNT_textTiny);
  snprintf(buf, sizeof(buf), "%d%%", (int)a->v[kBandWidth]);
  NT_drawText(leftX, valueY, buf, 15, kNT_textCentre, kNT_textTiny);

  NT_drawText(centreX, labelY, "DEPTH", 15, kNT_textCentre, kNT_textTiny);
  snprintf(buf, sizeof(buf), "%d%%", (int)a->v[kDepth]);
  NT_drawText(centreX, valueY, buf, 15, kNT_textCentre, kNT_textTiny);

  NT_drawText(rightX, labelY, "FORMANT", 15, kNT_textCentre, kNT_textTiny);
  snprintf(buf, sizeof(buf), "%d", (int)a->v[kFormant]);
  NT_drawText(rightX, valueY, buf, 15, kNT_textCentre, kNT_textTiny);

  const int bands = a->activeBands > 0 ? a->activeBands : 1;
  const int leftMargin = 4;
  const int rightMargin = 252;
  const int usableWidth = rightMargin - leftMargin;
  for (int i = 0; i < bands; ++i) {
    const int barLeft = leftMargin + (usableWidth * i) / bands;
    const int barRight = leftMargin + (usableWidth * (i + 1)) / bands - 1;
    const int innerLeft = barRight - barLeft > 2 ? barLeft + 1 : barLeft;
    const int innerRight = barRight - barLeft > 2 ? barRight - 1 : barRight;
    const int h = mapMeterToHeight(a->state ? a->state->meters[i] : 0.0f);
    NT_drawShapeI(kNT_box, barLeft, meterTop, barRight, meterBottom, 3);
    if (h > 0) {
      NT_drawShapeI(kNT_rectangle, innerLeft, (meterBottom - 1) - h,
                    innerRight, meterBottom - 1, 15);
    }
  }

  snprintf(buf, sizeof(buf), "BANDS %d", a->activeBands);
  NT_drawText(34, footerY, buf, 15, kNT_textCentre, kNT_textTiny);

  char decayBuf[24];
  formatDecay(decayBuf, sizeof(decayBuf), a->uiReleaseDisplay);
  snprintf(buf, sizeof(buf), "DEC %s", decayBuf);
  NT_drawText(96, footerY, buf, 15, kNT_textCentre, kNT_textTiny);

  snprintf(buf, sizeof(buf), "WET %d%%", a->uiWetDisplay);
  NT_drawText(160, footerY, buf, 15, kNT_textCentre, kNT_textTiny);

  snprintf(buf, sizeof(buf), "GAIN %d", a->uiOutputGainDisplay);
  NT_drawText(224, footerY, buf, 15, kNT_textCentre, kNT_textTiny);

  if (a->leftEncoderControlsDecay) {
    NT_drawShapeI(kNT_line, 64, 63, 128, 63, 15);
  } else {
    NT_drawShapeI(kNT_line, 4, 63, 64, 63, 15);
  }

  if (a->rightEncoderControlsGain) {
    NT_drawShapeI(kNT_line, 192, 63, 252, 63, 15);
  } else {
    NT_drawShapeI(kNT_line, 128, 63, 192, 63, 15);
  }

  return true;
}

uint32_t hasCustomUi(_NT_algorithm *) {
  return kNT_potL | kNT_potC | kNT_potR | kNT_encoderL | kNT_encoderR |
         kNT_encoderButtonL | kNT_encoderButtonR;
}

void customUi(_NT_algorithm *self, const _NT_uiData &data) {
  auto *a = (_vocoderAlgorithm *)self;

  if (data.controls & kNT_potL) {
    const int value = (int)(data.pots[0] * 100.0f);
    NT_setParameterFromUi(NT_algorithmIndex(self),
                          kBandWidth + NT_parameterOffset(), value);
  }
  if (data.controls & kNT_potC) {
    const int value = (int)(data.pots[1] * 200.0f);
    NT_setParameterFromUi(NT_algorithmIndex(self), kDepth + NT_parameterOffset(),
                          value);
  }
  if (data.controls & kNT_potR) {
    const int value = (int)(data.pots[2] * 480.0f - 240.0f);
    NT_setParameterFromUi(NT_algorithmIndex(self),
                          kFormant + NT_parameterOffset(), value);
  }

  if ((data.controls & kNT_encoderButtonL) &&
      !(data.lastButtons & kNT_encoderButtonL)) {
    a->leftEncoderControlsDecay = !a->leftEncoderControlsDecay;
  }

  if (data.encoders[0]) {
    if (a->leftEncoderControlsDecay) {
      int decay = a->uiReleaseDisplay +
                  data.encoders[0] * decayStepMs(a->uiReleaseDisplay);
      decay = decay < 10 ? 10 : (decay > 20000 ? 20000 : decay);
      a->uiReleaseDisplay = decay;
      NT_setParameterFromUi(NT_algorithmIndex(self),
                            kRelease + NT_parameterOffset(), decay);
    } else {
      int bandCount = a->v[kBandCount] + data.encoders[0];
      bandCount = bandCount < 4 ? 4 : (bandCount > 40 ? 40 : bandCount);
      NT_setParameterFromUi(NT_algorithmIndex(self),
                            kBandCount + NT_parameterOffset(), bandCount);
    }
  }

  if ((data.controls & kNT_encoderButtonR) &&
      !(data.lastButtons & kNT_encoderButtonR)) {
    a->rightEncoderControlsGain = !a->rightEncoderControlsGain;
  }

  if (data.encoders[1]) {
    if (a->rightEncoderControlsGain) {
      int gain = a->uiOutputGainDisplay + data.encoders[1] * 5;
      gain = gain < -600 ? -600 : (gain > 120 ? 120 : gain);
      a->uiOutputGainDisplay = gain;
      a->controls.targetOutputGainDb = gain * 0.1f;
      NT_setParameterFromUi(NT_algorithmIndex(self),
                            kPreGain + NT_parameterOffset(), gain);
    } else {
      int wet = a->uiWetDisplay + data.encoders[1] * 5;
      wet = wet < 0 ? 0 : (wet > 100 ? 100 : wet);
      a->uiWetDisplay = wet;
      NT_setParameterFromUi(NT_algorithmIndex(self),
                            kWet + NT_parameterOffset(), wet);
    }
  }
}
