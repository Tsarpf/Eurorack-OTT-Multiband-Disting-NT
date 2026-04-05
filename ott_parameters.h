/*  Stable indices: routing first, then every OTT control.  */
enum {
    /* routing */
    kIn, kStereo,
    kOut, kOutMode,

    /* Hi band */
    kHiDownThr, kHiUpThr,
    kHiDownRat, kHiUpRat,
    kHiPreGain, kHiPostGain,
    kHiAttack, kHiRelease,

    /* Mid band */
    kMidDownThr, kMidUpThr,
    kMidDownRat, kMidUpRat,
    kMidPreGain, kMidPostGain,
    kMidAttack, kMidRelease,

    /* Lo band */
    kLoDownThr, kLoUpThr,
    kLoDownRat, kLoUpRat,
    kLoPreGain, kLoPostGain,
    kLoAttack, kLoRelease,

    /* X-over  + global */
    kXoverLoMid, kXoverMidHi,
    kGlobalOut, kGlobalWet,

    kNumParams
};


static const uint8_t pageHi[]     = { kHiDownThr,kHiUpThr,kHiDownRat,kHiUpRat,kHiPreGain,kHiPostGain,kHiAttack,kHiRelease };
static const uint8_t pageMid[]    = { kMidDownThr,kMidUpThr,kMidDownRat,kMidUpRat,kMidPreGain,kMidPostGain,kMidAttack,kMidRelease };
static const uint8_t pageLow[]    = { kLoDownThr,kLoUpThr,kLoDownRat,kLoUpRat,kLoPreGain,kLoPostGain,kLoAttack,kLoRelease };
static const uint8_t pageGlobal[] = { kXoverLoMid,kXoverMidHi,kGlobalOut,kGlobalWet };
static const uint8_t pageRouting[] = { kIn, kStereo, kOut, kOutMode };

static const _NT_parameterPage pages[] = {
    { "High",    ARRAY_SIZE(pageHi),     pageHi     },
    { "Mid",     ARRAY_SIZE(pageMid),    pageMid    },
    { "Low",     ARRAY_SIZE(pageLow),    pageLow    },
    { "Global",  ARRAY_SIZE(pageGlobal), pageGlobal },
    { "Routing", ARRAY_SIZE(pageRouting), pageRouting }
};

static const _NT_parameterPages paramPages = {
    ARRAY_SIZE(pages), pages
};

static const char* const onOffEnum[] = { "Off", "On", nullptr };

/* helper macros – scaling10 means 0.1 dB or 0.1 % steps */
#define P(dbname,min,max,def,unit,sc) { dbname,min,max,def,unit,sc,nullptr }
static const _NT_parameter params[kNumParams] = {
    /* routing: mono by default; stereo = left+1 */
    NT_PARAMETER_AUDIO_INPUT("In", 1, 1)
    { "Stereo", 0, 1, 0, kNT_unitEnum, 0, onOffEnum },
    NT_PARAMETER_AUDIO_OUTPUT("Out", 1, 13)
    { "Out mode", 0, 1, 1, kNT_unitOutputMode, 0, nullptr },

    /*  High band  (dB, %, dB) */
    P("Hi/DownThr", -600,   0, -100, kNT_unitDb,       kNT_scaling10),
    P("Hi/UpThr",  -600,   0, -300, kNT_unitDb,       kNT_scaling10),
    P("Hi/DownRat", 100, 10000, 400, kNT_unitNone,     kNT_scaling100),
    P("Hi/UpRat",   100, 10000, 200, kNT_unitNone,     kNT_scaling100),
    P("Hi/PreGain", -240,  240,   0, kNT_unitDb,       kNT_scaling10),
    P("Hi/PostGain",-240,  240,   0, kNT_unitDb,       kNT_scaling10),
    P("Hi/Attack",     1,  5000, 135, kNT_unitMs,      kNT_scaling10),
    P("Hi/Release",   10, 20000,1320, kNT_unitMs,      kNT_scaling10),

    /*  Mid band  */
    P("Mid/DownThr", -600,   0, -100, kNT_unitDb,       kNT_scaling10),
    P("Mid/UpThr",  -600,   0, -300, kNT_unitDb,       kNT_scaling10),
    P("Mid/DownRat", 100, 10000, 400, kNT_unitNone,     kNT_scaling100),
    P("Mid/UpRat",   100, 10000, 200, kNT_unitNone,     kNT_scaling100),
    P("Mid/PreGain", -240,  240,   0, kNT_unitDb,       kNT_scaling10),
    P("Mid/PostGain",-240,  240,   0, kNT_unitDb,       kNT_scaling10),
    P("Mid/Attack",     1,  5000, 224, kNT_unitMs,      kNT_scaling10),
    P("Mid/Release",   10, 20000,2820, kNT_unitMs,      kNT_scaling10),

    /*  Low band  */
    P("Low/DownThr", -600,   0, -100, kNT_unitDb,       kNT_scaling10),
    P("Low/UpThr",  -600,   0, -300, kNT_unitDb,       kNT_scaling10),
    P("Low/DownRat", 100, 10000, 400, kNT_unitNone,     kNT_scaling100),
    P("Low/UpRat",   100, 10000, 200, kNT_unitNone,     kNT_scaling100),
    P("Low/PreGain", -240,  240,   0, kNT_unitDb,       kNT_scaling10),
    P("Low/PostGain",-240,  240,   0, kNT_unitDb,       kNT_scaling10),
    P("Low/Attack",     1,  5000, 478, kNT_unitMs,      kNT_scaling10),
    P("Low/Release",   10, 20000,2820, kNT_unitMs,      kNT_scaling10),

    /*  X-over & global  */
    P("Xover/LoMid",   40, 18000, 160,  kNT_unitHz,      0),
    P("Xover/MidHi",  100, 20000,2500,  kNT_unitHz,      0),
    P("Global/Out",  -240,  240,  170,  kNT_unitDb,      kNT_scaling10),
    P("Global/Wet",     0,  100,  100,  kNT_unitPercent, 0),
};
#undef P

