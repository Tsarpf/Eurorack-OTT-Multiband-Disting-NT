# Empirical summed-filterbank fit

This is a practical model of saved Live H1 measurements, not identification of Ableton's implementation. The data are 48kHz, 40 geometric centers from 30Hz to 18kHz, Precise, Depth0, Enhance off, Modulator carrier. Envelope behavior and Enhance are outside this fit.

Candidates include 1–4 identical biquad cascades and Butterworth bandpasses of 2–4 SOS stages (one Butterworth stage equals one ordinary bandpass). Every topology is evaluated with equal or alternating adjacent-band polarity and with or without digital bandwidth compensation.

Fit bands have even nearest-center indices within 500–8000Hz; odd indices are held out. Errors weight FFT bins by 1/f, approximately equal weight per log-frequency interval. Each candidate fits Q and a constant gain separately for each Width. The final law fits Width25–200 and is also evaluated at Width10. All data still come from one capture per Width, so held-out bands are frequency validation, not independent recordings. The 2.93Hz FFT resolution and finite settling limit low-frequency/narrow-band accuracy.

## Practical model

Use two non-identical SOS sections per band from a second-order Butterworth lowpass prototype transformed to bandpass. Sum bands with alternating signs. For width fraction w = Width/100, Q = 6.4577056/w and total post-sum gain = 0.1392751/sqrt(w). Every band has unity gain at its center before the sum.

A possible band-count extension is Q = 1.0592177/(log(maxHz/minHz)/(bands-1) * w). That extension is an inference; only the 40-band setting was fitted. The gain law is likewise verified only at 40 bands.

For each center fc: omega = 2*pi*fc/fs; qEff = Q*sin(omega)/omega; u = tan(omega/2); B = u/qEff; uLow = 2*u*u/(sqrt(B*B+4*u*u)+B); uHigh = uLow+B. Convert cutoffs to Hz with fs/pi*atan(uLow/uHigh), then scipy.signal.butter(2, [fLow,fHigh], btype='bandpass', fs=fs, output='sos'). `butterworth_sos()` is the executable recipe; the generated JSON includes an example coefficient pair.

For a C++ implementation, map each Butterworth prototype pole p through s² - B*p*s + u² = 0, apply z=(1+s)/(1-s), pair conjugate poles, place two zeros at z=+1 and two at z=-1 across the sections, and normalize the complete cascade to unity at fc. Compute coefficients in double precision and use stable SOS processing. The two section frequencies and Qs differ; duplicating an RBJ section does not give this Butterworth response.

The substitution s→(s²+ω₀²)/(s*BW) is documented by [SciPy lp2bp_zpk](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.lp2bp_zpk.html). The digital transform and absence of automatic prewarping are documented by [SciPy bilinear_zpk](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.bilinear_zpk.html). The sin(omega)/omega correction and Width/gain laws here are empirical modeling choices validated against the captured data.

## Individual full-midband fits

| Width % | Prototype Q | Post-sum gain | Shape RMSE dB |
|---:|---:|---:|---:|
| 10 | 63.0462 | 0.419182 | 0.3703 |
| 25 | 25.7593 | 0.276720 | 0.1606 |
| 50 | 12.9205 | 0.196772 | 0.0996 |
| 75 | 8.6286 | 0.160988 | 0.0803 |
| 100 | 6.4881 | 0.139561 | 0.0801 |
| 150 | 4.2952 | 0.113905 | 0.0871 |
| 200 | 3.2494 | 0.097404 | 0.0908 |

## Stability and ringing

All tested SOS pole radii remain below one after float32 coefficient quantization. This checks coefficient stability, not runtime rounding noise, denormals, coefficient-update transients, CPU cost, or headroom. Narrow bands necessarily ring longer; steeper skirts do not mean faster settling.

At Width10 the slowest 30Hz pole has radius 0.999978618, with an asymptotic 60dB decay proxy of 6.73s. At Width100 it is 0.71s. Cascading two sections adds state and filter phase; keep coefficient updates smooth and validate impulse tails and CPU use in the native renderer.

The saved complex H1 is also checked against the selected model after fitting a global delay and constant phase. Agreement supports the practical model but does not establish internal structure. No envelope-bank topology, alternate band count, sample rate, or dynamic behavior is inferred from Depth0 measurements.

Reproduce: `PYTHONPATH=/tmp/vocoder-compare-libs python3 tools/live_probe/fit_filterbank.py --comparison <source-comparison-root> --output-dir <new-comparison-root>/analysis`
