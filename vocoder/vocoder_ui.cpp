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

void setupUi(_NT_algorithm *self, _NT_float3 &pots) {
  auto *a = (_vocoderAlgorithm *)self;
  pots[0] = vocoderClamp((float)a->v[kBandWidth] / 100.0f, 0.0f, 1.0f);
  pots[1] = vocoderClamp((float)a->v[kDepth] / 100.0f, 0.0f, 1.0f);
  pots[2] =
      vocoderClamp(((float)a->v[kFormant] + 240.0f) / 480.0f, 0.0f, 1.0f);
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
  NT_drawText(64, footerY, buf, 15, kNT_textCentre, kNT_textTiny);

  snprintf(buf, sizeof(buf), "WET %d%%", (int)a->v[kWet]);
  NT_drawText(192, footerY, buf, 15, kNT_textCentre, kNT_textTiny);

  return true;
}

uint32_t hasCustomUi(_NT_algorithm *) {
  return kNT_potL | kNT_potC | kNT_potR | kNT_encoderL | kNT_encoderR;
}

void customUi(_NT_algorithm *self, const _NT_uiData &data) {
  auto *a = (_vocoderAlgorithm *)self;

  if (data.controls & kNT_potL) {
    NT_setParameterFromUi(NT_algorithmIndex(self),
                          kBandWidth + NT_parameterOffset(),
                          (int)(data.pots[0] * 100.0f));
  }
  if (data.controls & kNT_potC) {
    NT_setParameterFromUi(NT_algorithmIndex(self), kDepth + NT_parameterOffset(),
                          (int)(data.pots[1] * 100.0f));
  }
  if (data.controls & kNT_potR) {
    NT_setParameterFromUi(NT_algorithmIndex(self),
                          kFormant + NT_parameterOffset(),
                          (int)(data.pots[2] * 480.0f - 240.0f));
  }

  if (data.encoders[0]) {
    int bandCount = a->v[kBandCount] + data.encoders[0];
    bandCount = bandCount < 4 ? 4 : (bandCount > 40 ? 40 : bandCount);
    NT_setParameterFromUi(NT_algorithmIndex(self),
                          kBandCount + NT_parameterOffset(), bandCount);
  }

  if (data.encoders[1]) {
    int wet = a->v[kWet] + data.encoders[1] * 5;
    wet = wet < 0 ? 0 : (wet > 100 ? 100 : wet);
    NT_setParameterFromUi(NT_algorithmIndex(self), kWet + NT_parameterOffset(),
                          wet);
  }
}
