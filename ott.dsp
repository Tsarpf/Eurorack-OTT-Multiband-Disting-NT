import("stdfaust.lib");

//===== CROSSOVER FREQS ======================================
c1 = hslider("Xover/LowMidFreq[unit:Hz]", 160, 40, 1000, 1);
c2 = hslider("Xover/MidHighFreq[unit:Hz]", 2500, 300, 16000, 1);

//===== ATTACK / RELEASE (ms converted to seconds) ============================
L_att = hslider("Low/Attack[unit:ms]", 47.8, 0.1, 500, 0.1) / 1000.0;
L_rel = hslider("Low/Release[unit:ms]", 282, 1, 2000, 0.1) / 1000.0;
M_att = hslider("Mid/Attack[unit:ms]", 22.4, 0.1, 500, 0.1) / 1000.0;
M_rel = hslider("Mid/Release[unit:ms]", 282, 1, 2000, 0.1) / 1000.0;
H_att = hslider("High/Attack[unit:ms]", 13.5, 0.1, 500, 0.1) / 1000.0;
H_rel = hslider("High/Release[unit:ms]", 132, 1, 2000, 0.1) / 1000.0;

//===== LOW BAND =============================================
L_thd  = hslider("Low/DownThr[dB]", -10, -60, 0, 0.1);
L_thu  = hslider("Low/UpThr[dB]"  , -30, -60, 0, 0.1);
L_ratd = hslider("Low/DownRat"    ,   4,   1, 100, 0.01);
L_ratu = hslider("Low/UpRat"      ,   2,   1, 100, 0.01);
L_pre  = hslider("Low/PreGain[dB]" ,   0, -24, 24, 0.1);
L_post = hslider("Low/PostGain[dB]",   0, -24, 24, 0.1);

//===== MID BAND =============================================
M_thd  = hslider("Mid/DownThr[dB]", -10, -60, 0, 0.1);
M_thu  = hslider("Mid/UpThr[dB]"  , -30, -60, 0, 0.1);
M_ratd = hslider("Mid/DownRat"    ,   4,   1, 100, 0.01);
M_ratu = hslider("Mid/UpRat"      ,   2,   1, 100, 0.01);
M_pre  = hslider("Mid/PreGain[dB]" ,   0, -24, 24, 0.1);
M_post = hslider("Mid/PostGain[dB]",   0, -24, 24, 0.1);

//===== HIGH BAND ============================================
H_thd  = hslider("High/DownThr[dB]", -10, -60, 0, 0.1);
H_thu  = hslider("High/UpThr[dB]"  , -30, -60, 0, 0.1);
H_ratd = hslider("High/DownRat"    ,   4,   1, 100, 0.01);
H_ratu = hslider("High/UpRat"      ,   2,   1, 100, 0.01);
H_pre  = hslider("High/PreGain[dB]" ,   0, -24, 24, 0.1);
H_post = hslider("High/PostGain[dB]",   0, -24, 24, 0.1);

//===== GLOBAL MIX / OUTPUT =================================
wet  = hslider("Global/Wet[unit:%]", 100, 0, 100, 0.1) / 100.0;
outg = hslider("Global/OutGain[dB]",   0, -24, 24, 0.1);

//===== HELPERS ==============================================
db2lin(x) = pow(10, x/20);
// envelope follower with attack and release times
env_foll(at, rt, x) = abs(x) : an.amp_follower_ar(at, rt);

// bidirectional gain (old-syntax safe)
ud_gain(env, td, tu, rd, ru) = g with {
  thrD   = db2lin(td);
  thrU   = db2lin(tu);
  e      = max(env, 1e-9);
  gD_raw = pow(thrD / e , 1.0 - 1.0/rd);
  gU_raw = pow(e  / thrU, (1.0/ru) - 1.0);
  gDown  = min(1.0, gD_raw);
  gUp    = max(1.0, gU_raw);
  g      = gDown * gUp;
};  // <- semicolon required for Faust 2.37

// per-band processor
band(sig, td, tu, rd, ru, pre, post, at, rt) = y with {
  pg   = db2lin(pre);
  env  = env_foll(at, rt, sig * pg);
  comp = ud_gain(env, td, tu, rd, ru);
  y    = sig * pg * comp * db2lin(post);
};

// 2-pole Butterworth crossovers
lp = fi.lowpass(2, c1);
hp = fi.highpass(2, c2);

// per-channel chain
chain(x) = y with {
  lo_i = x : lp;
  hi_i = x : hp;
  mid_i= x - lo_i - hi_i;

  lo_p  = band(lo_i , L_thd,L_thu,L_ratd,L_ratu,L_pre,L_post, L_att,L_rel);
  mid_p = band(mid_i, M_thd,M_thu,M_ratd,M_ratu,M_pre,M_post, M_att,M_rel);
  hi_p  = band(hi_i , H_thd,H_thu,H_ratd,H_ratu,H_pre,H_post, H_att,H_rel);

  wetmix = lo_p + mid_p + hi_p;
  y = (wetmix * wet + x * (1 - wet)) * db2lin(outg);
};  // closes the outer with

//===== STEREO =================================================
process = _,_ : chain, chain;

