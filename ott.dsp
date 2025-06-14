import("stdfaust.lib");

// -----------------------------------------------------------------------------
// UI parameters (19 total)
// -----------------------------------------------------------------------------
// Crossovers
c1  = hslider("[1]LowMidFreq[unit:Hz]", 120,   20,  1000,  1);
c2  = hslider("[1]MidHighFreq[unit:Hz]", 2500, 300, 16000, 1);

// Per‑band controls (prefix L_, M_, H_)
// ↓ threshold, ↑ threshold, ↓ ratio, ↑ ratio, makeup gain
// (Faust unit annotation purely cosmetic – values will be mapped in the C++ UI)
L_td = hslider("[2]Low:ThrDown[dB]", -40, -60, 0, 0.1);
L_tu = hslider("[2]Low:ThrUp[dB]",  -20, -60, 0, 0.1);
L_rd = hslider("[2]Low:RatDown",  4, 1, 12, 0.01);
L_ru = hslider("[2]Low:RatUp",    2, 1, 12, 0.01);
L_g  = hslider("[2]Low:Makeup[dB]", 0, -24, 24, 0.1);

M_td = hslider("[3]Mid:ThrDown[dB]", -40, -60, 0, 0.1);
M_tu = hslider("[3]Mid:ThrUp[dB]",  -20, -60, 0, 0.1);
M_rd = hslider("[3]Mid:RatDown",  4, 1, 12, 0.01);
M_ru = hslider("[3]Mid:RatUp",    2, 1, 12, 0.01);
M_g  = hslider("[3]Mid:Makeup[dB]", 0, -24, 24, 0.1);

H_td = hslider("[4]High:ThrDown[dB]", -40, -60, 0, 0.1);
H_tu = hslider("[4]High:ThrUp[dB]",  -20, -60, 0, 0.1);
H_rd = hslider("[4]High:RatDown",  4, 1, 12, 0.01);
H_ru = hslider("[4]High:RatUp",    2, 1, 12, 0.01);
H_g  = hslider("[4]High:Makeup[dB]", 0, -24, 24, 0.1);

// Global
wet  = hslider("[5]WetDry[%]", 100, 0, 100, 0.1)/100.0;
outg = hslider("[5]OutputGain[dB]", 0, -24, 24, 0.1);

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
compress(name, thrDn, thrUp, ratDn, ratUp) =
    ba.dyn_peak_compressor(name, thrDn, ratDn, 0.01, 0.1) * ((_:dcblocker) : _);

make_band(name, sig, td, tu, rd, ru, g) =
    env = ma.ADF(sig : abs) : si.smooth(0.01);
    gainDn = (env > td) ? pow(10, (td - env) * (1.0 - 1.0/rd) / 20) : 1;
    gainUp = (env < tu) ? pow(10, (tu - env) * (1.0 - 1.0/ru) / 20) : 1;
    totalGain = gainDn * gainUp * pow(10, g/20);
    sig * totalGain;

process = stereo_in :> deinterleave : (*(0.5),*(0.5)) <: splitBands :> _,_ :> sum * pow(10,outg/20);

splitBands(l, r) =
    // 2‑pole LR‑24 acoustic‑neutral cross‑overs
    low  = fi.lowpass(2, c1);
    high = fi.highpass(2, c2);
    mid  = l + r - low - high;

    make_band("L", low,  L_td, L_tu, L_rd, L_ru, L_g),
    make_band("M", mid,  M_td, M_tu, M_rd, M_ru, M_g),
    make_band("H", high, H_td, H_tu, H_rd, H_ru, H_g);