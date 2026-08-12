#ifndef VOCODER_PARAMETERS_H
#define VOCODER_PARAMETERS_H

#include <cstddef>
#include <distingnt/api.h>

enum {
  /* routing */
  kInCarrier,
  kCarrierStereo,
  kInModulator,
  kModulatorStereo,
  kOut,
  kOutMode,

  /* Parameters */
  kBandCount,
  kBandWidth,
  kDepth,
  kFormant,
  kMinFreq,
  kMaxFreq,
  kAttack,
  kRelease,
  kEnhance,
  kWet,
  kPreGain,

  /* Append display-only parameters to preserve existing preset indices. */
  kOutRight,

  kNumParams
};

static const char *const onOffEnum[] = {"Off", "On", nullptr};

static const uint8_t pageMain[] = {kBandCount, kBandWidth, kDepth, kFormant,
                                   kWet, kPreGain};
static const uint8_t pageFreq[] = {kMinFreq, kMaxFreq};
static const uint8_t pageEnv[] = {kAttack, kRelease};
static const uint8_t pageRouting[] = {kInCarrier, kCarrierStereo, kInModulator,
                                      kModulatorStereo, kOut, kOutMode,
                                      kOutRight};

static const _NT_parameterPage pages[] = {
    {"Main", ARRAY_SIZE(pageMain), 0, {0, 0}, pageMain},
    {"Freq", ARRAY_SIZE(pageFreq), 0, {0, 0}, pageFreq},
    {"Env", ARRAY_SIZE(pageEnv), 0, {0, 0}, pageEnv},
    {"Routing", ARRAY_SIZE(pageRouting), 0, {0, 0}, pageRouting}};

static const _NT_parameterPages paramPages = {ARRAY_SIZE(pages), pages};

#define P(dbname, min, max, def, unit, sc)                                     \
  {dbname, min, max, def, unit, sc, nullptr}

static const _NT_parameter parameters[kNumParams] = {
    /* routing */
    NT_PARAMETER_AUDIO_INPUT("Carrier", 1, 1)
        { "Carrier stereo", 0, 1, 0, kNT_unitEnum, 0, onOffEnum },
        NT_PARAMETER_AUDIO_INPUT("Modulator", 1, 2)
        { "Mod stereo", 0, 1, 0, kNT_unitEnum, 0, onOffEnum },
        NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output", 1, 13)

    /* Controls */
    P("Bands", 4, 40, 16, kNT_unitNone, 0),
    P("Width", 0, 100, 50, kNT_unitPercent, 0),
    P("Depth", 0, 800, 100, kNT_unitPercent, 0),
    P("Formant", -240, 240, 0, kNT_unitSemitones, kNT_scaling10),
    P("Min Freq", 30, 1000, 30, kNT_unitHz, 0),
    P("Max Freq", 2000, 20000, 18000, kNT_unitHz, 0),
    P("Attack", 1, 500, 10, kNT_unitMs, 0),
    P("Decay", 1, 1000, 100, kNT_unitMs, 0),
    /* Retained but not paged so existing presets keep their parameter layout. */
    P("Reserved", 0, 1, 0, kNT_unitEnum, 0),
    P("Wet", 0, 100, 100, kNT_unitPercent, 0),
    P("Pre", -600, 120, 0, kNT_unitDb, kNT_scaling10),
    P("Right output", 0, 0, 0, kNT_unitHasStrings, 0),
};

#undef P

#endif // VOCODER_PARAMETERS_H
