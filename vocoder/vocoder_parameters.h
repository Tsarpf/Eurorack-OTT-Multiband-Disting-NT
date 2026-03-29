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

  kNumParams
};

static const char *const onOffEnum[] = {"Off", "On", nullptr};

static const uint8_t pageMain[] = {kBandCount, kBandWidth, kDepth, kFormant,
                                   kWet, kPreGain};
static const uint8_t pageFreq[] = {kMinFreq, kMaxFreq, kEnhance};
static const uint8_t pageEnv[] = {kAttack, kRelease};
static const uint8_t pageRouting[] = {kInCarrier, kCarrierStereo, kInModulator,
                                      kModulatorStereo, kOut, kOutMode};

static const _NT_parameterPage pages[] = {
    {"Main", ARRAY_SIZE(pageMain), pageMain},
    {"Freq", ARRAY_SIZE(pageFreq), pageFreq},
    {"Env", ARRAY_SIZE(pageEnv), pageEnv},
    {"Routing", ARRAY_SIZE(pageRouting), pageRouting}};

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
    P("Depth", 0, 200, 100, kNT_unitPercent, 0),
    P("Formant", -240, 240, 0, kNT_unitSemitones, kNT_scaling10),
    P("Min Freq", 20, 1000, 35, kNT_unitHz, 0),
    P("Max Freq", 2000, 20000, 18000, kNT_unitHz, 0),
    P("Attack", 1, 500, 10, kNT_unitMs, 0),
    P("Release", 10, 2000, 100, kNT_unitMs, 0),
    P("Enhance", 0, 1, 0, kNT_unitEnum, 0), // Off/On toggle
    P("Wet", 0, 100, 100, kNT_unitPercent, 0),
    P("Pre", -600, 120, 0, kNT_unitNone, 0),
};

#undef P

#endif // VOCODER_PARAMETERS_H
