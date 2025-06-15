/*  Stable indices: routing first, then every OTT control.  */
enum {
    /* routing */
    kInL, kInR,
    kOutL, kOutLMode,
    kOutR, kOutRMode,

    /* Hi band */
    kHiDownThr, kHiUpThr,
    kHiDownRat, kHiUpRat,
    kHiMakeup,

    /* Mid band */
    kMidDownThr, kMidUpThr,
    kMidDownRat, kMidUpRat,
    kMidMakeup,

    /* Lo band */
    kLoDownThr, kLoUpThr,
    kLoDownRat, kLoUpRat,
    kLoMakeup,

    /* X-over  + global */
    kXoverLoMid, kXoverMidHi,
    kGlobalOut, kGlobalWet,

    kNumParams
};


static const uint8_t pageHi[]     = { kHiDownThr,kHiUpThr,kHiDownRat,kHiUpRat,kHiMakeup };
static const uint8_t pageMid[]    = { kMidDownThr,kMidUpThr,kMidDownRat,kMidUpRat,kMidMakeup };
static const uint8_t pageLow[]    = { kLoDownThr,kLoUpThr,kLoDownRat,kLoUpRat,kLoMakeup };
static const uint8_t pageGlobal[] = { kXoverLoMid,kXoverMidHi,kGlobalOut,kGlobalWet };
static const uint8_t pageRouting[] = { kInL,kInR,kOutL,kOutLMode,kOutR,kOutRMode };

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

/* helper macros – scaling10 means 0.1 dB or 0.1 % steps */
#define P(dbname,min,max,def,unit,sc) { dbname,min,max,def,unit,sc,nullptr }
static const _NT_parameter params[kNumParams] = {
    /* routing */
    NT_PARAMETER_AUDIO_INPUT ("In L", 1, 1)
    NT_PARAMETER_AUDIO_INPUT ("In R", 2, 2)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Out L", 1, 13)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Out R", 2, 14)

    /*  High band  (dB, %, dB) */
    P("Hi/DownThr", -600,   0, -120, kNT_unitDb,       kNT_scaling10),
    P("Hi/UpThr",      0,  400, 120, kNT_unitDb,       kNT_scaling10),
    P("Hi/DownRat",   10, 1000, 500, kNT_unitPercent,  kNT_scaling10),
    P("Hi/UpRat",     10, 8000,2000, kNT_unitPercent,  kNT_scaling10),
    P("Hi/Makeup",  -240,  240,   0, kNT_unitDb,       kNT_scaling10),

    /*  Mid band  */
    P("Mid/DownThr", -600,   0, -120, kNT_unitDb,       kNT_scaling10),
    P("Mid/UpThr",      0,  400, 120, kNT_unitDb,       kNT_scaling10),
    P("Mid/DownRat",   10, 1000, 500, kNT_unitPercent,  kNT_scaling10),
    P("Mid/UpRat",     10, 8000,2000, kNT_unitPercent,  kNT_scaling10),
    P("Mid/Makeup",  -240,  240,   0, kNT_unitDb,       kNT_scaling10),

    /*  Low band  */
    P("Low/DownThr", -600,   0, -120, kNT_unitDb,       kNT_scaling10),
    P("Low/UpThr",      0,  400, 120, kNT_unitDb,       kNT_scaling10),
    P("Low/DownRat",   10, 1000, 500, kNT_unitPercent,  kNT_scaling10),
    P("Low/UpRat",     10, 8000,2000, kNT_unitPercent,  kNT_scaling10),
    P("Low/Makeup",  -240,  240,   0, kNT_unitDb,       kNT_scaling10),

    /*  X-over & global  */
    P("Xover/LoMid",   40, 18000, 400,  kNT_unitHz,      0),
    P("Xover/MidHi",  100, 20000,2500,  kNT_unitHz,      0),
    P("Global/Out",  -240,  240,    0,  kNT_unitDb,      kNT_scaling10),
    P("Global/Wet",     0,  100,  100,  kNT_unitPercent, 0),
};
#undef P

