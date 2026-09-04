#!/usr/bin/env python3
"""Fit practical digital filterbanks to saved Live Width measurements.

Reads only width_spectra.npz; never controls Live or edits the native DSP.
Requires NumPy, SciPy and Matplotlib. The fit is a model of measured transfer
functions, not identification of Ableton's implementation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from scipy import optimize, signal


FS = 48000
WIDTHS = (10, 25, 50, 75, 100, 150, 200)
CENTERS = np.geomspace(30.0, 18000.0, 40)
SPACING = np.log(CENTERS[-1] / CENTERS[0]) / (len(CENTERS) - 1)
SOURCES = {
    "butterworth": "https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.butter.html",
    "lowpass_to_bandpass": "https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.lp2bp_zpk.html",
    "bilinear_transform": "https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.bilinear_zpk.html",
}


@dataclass(frozen=True)
class Candidate:
    family: str
    stages: int
    alternating: bool
    compensated: bool

    @property
    def name(self) -> str:
        return "{}_{}_{}_{}".format(
            self.family, self.stages,
            "alternating" if self.alternating else "same",
            "compensated" if self.compensated else "uncompensated")


SELECTED = Candidate("butterworth", 2, True, True)


class Response:
    """Evaluate the LP prototype after BP and bilinear substitutions."""

    def __init__(self, frequency: np.ndarray, candidate: Candidate):
        self.candidate = candidate
        u = np.tan(np.pi * frequency / FS)[None, :]
        uc = np.tan(np.pi * CENTERS / FS)[:, None]
        self.coordinate = 1j * (u / uc - uc / u)
        if candidate.compensated:
            omega = 2 * np.pi * CENTERS / FS
            self.coordinate *= (np.sin(omega) / omega)[:, None]
        self.sign = ((-1.0) ** np.arange(len(CENTERS)) if candidate.alternating
                     else np.ones(len(CENTERS)))[:, None]
        self.poles = signal.buttap(candidate.stages)[1]

    def complex(self, q: float) -> np.ndarray:
        t = q * self.coordinate
        if self.candidate.family == "cascade":
            bands = (1 + t) ** -self.candidate.stages
        else:
            bands = np.full(t.shape, np.prod(-self.poles), dtype=np.complex128)
            for pole in self.poles:
                bands /= t - pole
        return np.sum(self.sign * bands, axis=0)

    def db(self, q: float) -> np.ndarray:
        return 20 * np.log10(np.maximum(np.abs(self.complex(q)), 1e-15))


def weights(frequency: np.ndarray) -> np.ndarray:
    result = 1 / frequency
    return result / result.sum()


def metrics(model: np.ndarray, measured: np.ndarray, frequency: np.ndarray,
            gain_db: float | None = None) -> dict:
    weight = weights(frequency)
    delta = model - measured
    optimal_gain = -float(np.sum(delta * weight))
    if gain_db is None:
        gain_db = optimal_gain
    return {
        "shape_rmse_db": float(np.sqrt(np.sum(weight * (delta + optimal_gain) ** 2))),
        "gain_db": optimal_gain,
        "gain_linear": float(10 ** (optimal_gain / 20)),
        "rmse_with_training_gain_db": float(np.sqrt(np.sum(weight * (delta + gain_db) ** 2))),
    }


def fit_candidate(candidate: Candidate, frequency: np.ndarray,
                  measured: dict[int, np.ndarray], train: np.ndarray,
                  held: np.ndarray, low: np.ndarray, high: np.ndarray) -> dict:
    train_response = Response(frequency[train], candidate)
    train_weight = weights(frequency[train])
    # Several broad-width minima exist. Search a dense log-Q grid, then refine
    # the best local basins; a single bounded optimization can pick the wrong one.
    grid = np.log(np.geomspace(0.2, 300, 180))
    grid_db = np.asarray([train_response.db(np.exp(q)) for q in grid])
    all_response = Response(frequency, candidate)
    rows = {}
    for width in WIDTHS:
        target = measured[width][train]
        difference = grid_db - target
        difference -= np.sum(difference * train_weight, axis=1)[:, None]
        grid_errors = np.sqrt(np.sum(train_weight * difference ** 2, axis=1))
        minima = signal.find_peaks(-grid_errors)[0].tolist() + [0, len(grid) - 1]
        minima = sorted(minima, key=lambda i: grid_errors[i])[:4]

        def objective(log_q: float) -> float:
            return metrics(train_response.db(np.exp(log_q)), target,
                           frequency[train])["shape_rmse_db"]

        solutions = [optimize.minimize_scalar(
            objective, bounds=(grid[max(0, i - 1)], grid[min(len(grid) - 1, i + 1)]),
            method="bounded", options={"xatol": 1e-6}) for i in minima]
        best = min(solutions, key=lambda fit: fit.fun)
        q = float(np.exp(best.x))
        model = all_response.db(q)
        training = metrics(model[train], measured[width][train], frequency[train])
        rows[str(width)] = {
            "q": q, "training": training,
            "held_out_bands": metrics(model[held], measured[width][held],
                                       frequency[held], training["gain_db"]),
            "low_frequency_extrapolation": metrics(model[low], measured[width][low],
                                                     frequency[low], training["gain_db"]),
            "high_frequency_extrapolation": metrics(model[high], measured[width][high],
                                                      frequency[high], training["gain_db"]),
        }
    narrow_errors = [rows[str(width)]["held_out_bands"]["shape_rmse_db"]
                     for width in (10, 25, 50, 75)]
    return {
        **asdict(candidate), "name": candidate.name, "widths": rows,
        "narrow_mid_held_out_rmse_db": float(np.sqrt(np.mean(np.square(narrow_errors)))),
        "all_widths_held_out_rmse_db": float(np.sqrt(np.mean([
            row["held_out_bands"]["shape_rmse_db"] ** 2 for row in rows.values()]))),
    }


def butterworth_sos(center: float, q: float) -> np.ndarray:
    """Exact coefficient recipe used by the selected analytical response."""
    omega = 2 * np.pi * center / FS
    effective_q = q * np.sin(omega) / omega
    u = np.tan(omega / 2)
    bandwidth = u / effective_q
    # Rationalized form avoids cancellation at low Q / large bandwidth.
    low = 2 * u * u / (np.sqrt(bandwidth * bandwidth + 4 * u * u) + bandwidth)
    high = low + bandwidth
    cutoffs = FS / np.pi * np.arctan([low, high])
    return signal.butter(2, cutoffs, btype="bandpass", fs=FS, output="sos")


def phase_metrics(model: np.ndarray, measured: np.ndarray, frequency: np.ndarray) -> dict:
    phase = np.unwrap(np.angle(measured / model))
    polynomial = np.polyfit(frequency, phase, 1, w=np.sqrt(weights(frequency)))
    residual = phase - np.polyval(polynomial, frequency)
    return {
        "fitted_delay_samples": float(-polynomial[0] * FS / (2 * np.pi)),
        "constant_phase_degrees": float(np.rad2deg(np.angle(np.exp(1j * polynomial[1])))),
        "phase_residual_rms_degrees": float(np.rad2deg(np.sqrt(
            np.sum(weights(frequency) * residual ** 2)))),
        "scope": "Complex H1 comparison after fitting one bulk delay and constant phase; neither was used to fit magnitude",
    }


def fit_law(frequency: np.ndarray, measured: dict[int, np.ndarray],
            h1: dict[int, np.ndarray], train: np.ndarray, held: np.ndarray) -> tuple[dict, dict]:
    response = Response(frequency, SELECTED)
    train_response = Response(frequency[train], SELECTED)
    main = (frequency >= 500) & (frequency <= 8000)
    main_response = Response(frequency[main], SELECTED)
    # Width10 has only 4.5 FFT bins across the ~1kHz peak and is deliberately
    # excluded from estimating a continuous law. It remains an evaluated case.
    law_widths = WIDTHS[1:]

    def objective(q_at_100: float) -> float:
        errors = [metrics(train_response.db(q_at_100 / (width / 100)),
                          measured[width][train], frequency[train])["shape_rmse_db"]
                  for width in law_widths]
        return float(np.sqrt(np.mean(np.square(errors))))

    optimum = optimize.minimize_scalar(objective, bounds=(6.1, 6.9), method="bounded")
    q_at_100 = float(optimum.x)
    gain_at_100_db = float(np.mean([
        metrics(train_response.db(q_at_100 / (width / 100)), measured[width][train],
                frequency[train])["gain_db"] + 10 * np.log10(width / 100)
        for width in law_widths]))
    result = {
        "q_at_width_100": q_at_100,
        "q_times_log_band_spacing": q_at_100 * SPACING,
        "gain_at_width_100_db": gain_at_100_db,
        "gain_at_width_100_linear": float(10 ** (gain_at_100_db / 20)),
        "q_law": "Q = q_at_width_100 / (width_percent / 100)",
        "gain_law": "gain = gain_at_width_100_linear / sqrt(width_percent / 100)",
        "fit_widths": list(law_widths), "evaluations": {}, "individual_fits": {},
    }
    spectra = {"frequency": frequency}
    for width in WIDTHS:
        q = q_at_100 / (width / 100)
        gain = gain_at_100_db - 10 * np.log10(width / 100)
        model = response.db(q)
        spectra[f"live_{width}_db"] = measured[width]
        spectra[f"law_{width}_db"] = model + gain
        result["evaluations"][str(width)] = {
            "q": q, "gain_db": gain,
            "training": metrics(model[train], measured[width][train], frequency[train], gain),
            "held_out_bands": metrics(model[held], measured[width][held], frequency[held], gain),
            "all_midband": metrics(model[main], measured[width][main], frequency[main], gain),
            "full_100_10000_hz": metrics(model, measured[width], frequency, gain),
            "phase_midband": phase_metrics(response.complex(q)[main], h1[width][main], frequency[main]),
        }
        fit = optimize.minimize_scalar(
            lambda value: metrics(main_response.db(value), measured[width][main],
                                  frequency[main])["shape_rmse_db"],
            bounds=(q * 0.8, q * 1.2), method="bounded")
        result["individual_fits"][str(width)] = {
            "q": float(fit.x),
            **metrics(main_response.db(fit.x), measured[width][main], frequency[main]),
        }
    return result, spectra


def verify_recipe(q_at_100: float) -> dict:
    sample_frequency = np.geomspace(20, 23000, 4000)
    analytical = Response(sample_frequency, SELECTED)
    rows = {}
    max_error = 0.0
    for width in WIDTHS:
        q = q_at_100 / (width / 100)
        bank = np.zeros(len(sample_frequency), dtype=np.complex128)
        radii, float_radii = [], []
        for index, center in enumerate(CENTERS):
            sos = butterworth_sos(center, q)
            _, h = signal.sosfreqz(sos, worN=sample_frequency, fs=FS)
            bank += (-1) ** index * h
            radii.extend(np.abs(signal.sos2zpk(sos)[1]))
            float_radii.extend(np.abs(signal.sos2zpk(sos.astype(np.float32).astype(float))[1]))
        error = float(np.max(np.abs(bank - analytical.complex(q))))
        max_error = max(max_error, error)
        radius = float(max(radii))
        rows[str(width)] = {
            "max_pole_radius_float64": radius,
            "max_pole_radius_float32_coefficients": float(max(float_radii)),
            "slowest_pole_60db_decay_seconds": float(np.log(0.001) / np.log(radius) / FS),
            "scipy_analytical_max_complex_error": error,
        }
    if max_error > 1e-7 or any(row["max_pole_radius_float32_coefficients"] >= 1
                             for row in rows.values()):
        raise ValueError("Coefficient recipe equivalence or stability verification failed")
    return {
        "widths": rows,
        "note": "Pole decay is an asymptotic ringing proxy, not a measured settling time or a proof of runtime float32 numerical behavior",
        "example_center_hz": float(CENTERS[21]),
        "example_width_25_scipy_sos": butterworth_sos(CENTERS[21], q_at_100 / 0.25).tolist(),
    }


def plot(destination: Path, result: dict, spectra: dict) -> None:
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(3, 2, figsize=(14, 12), constrained_layout=True)
    frequency = spectra["frequency"]
    colors = {10: "#aa4499", 25: "#4477aa", 50: "#228833", 75: "#cc6677",
              100: "#ee7733", 150: "#66aaaa", 200: "#777777"}
    for width in (10, 25, 50):
        mask = (frequency >= 700) & (frequency <= 1250)
        reference = spectra[f"live_{width}_db"]
        model = spectra[f"law_{width}_db"]
        norm = np.max(reference[mask])
        axes[0, 0].plot(frequency[mask], reference[mask] - norm, color=colors[width], label=f"Live {width}%")
        axes[0, 0].plot(frequency[mask], model[mask] - norm, color=colors[width], ls="--", label=f"Model {width}%")
    axes[0, 0].set(title="Midband skirts and interband notches", xlabel="Frequency (Hz)",
                   ylabel="dB relative to each Live curve's local peak", ylim=(-65, 2))
    axes[0, 0].legend(ncol=2, fontsize=8)

    for width in WIDTHS:
        mask = (frequency >= 100) & (frequency <= 10000)
        error = spectra[f"law_{width}_db"] - spectra[f"live_{width}_db"]
        axes[0, 1].semilogx(frequency[mask], error[mask], color=colors[width], lw=0.8, label=f"{width}%")
    axes[0, 1].axvspan(100, 500, color="#cccccc", alpha=0.2)
    axes[0, 1].axvspan(8000, 10000, color="#cccccc", alpha=0.2)
    axes[0, 1].set(title="Fixed Q/gain law: model minus measured H1", xlabel="Frequency (Hz)",
                   ylabel="Magnitude error (dB)", xlim=(100, 10000), ylim=(-3, 3))
    axes[0, 1].legend(ncol=4, fontsize=8)

    for family, linestyle in (("cascade", "-"), ("butterworth", "--")):
        for stages in range(1 if family == "cascade" else 2, 5):
            for alternating, marker in ((False, "o"), (True, "s")):
                name = Candidate(family, stages, alternating, True).name
                row = next(c for c in result["candidates"] if c["name"] == name)
                value = [row["widths"][str(w)]["held_out_bands"]["shape_rmse_db"] for w in WIDTHS]
                label = f"{'Cascade' if family == 'cascade' else 'Butter BP'} {stages}, {'±' if alternating else '+'}"
                axes[1, 0].semilogy(WIDTHS, value, linestyle, marker=marker, ms=3, lw=0.8, label=label)
    axes[1, 0].set(title="Held-out bands: topology comparison (warping compensated)",
                   xlabel="Live Width (%)", ylabel="Gain-normalized RMSE (dB)")
    axes[1, 0].legend(ncol=2, fontsize=7)

    for compensated, style in ((False, "--"), (True, "-")):
        name = Candidate("butterworth", 2, True, compensated).name
        row = next(c for c in result["candidates"] if c["name"] == name)
        value = [row["widths"][str(w)]["held_out_bands"]["shape_rmse_db"] for w in WIDTHS]
        axes[1, 1].plot(WIDTHS, value, style, marker="o", label="Compensated" if compensated else "Uncompensated")
    axes[1, 1].set(title="Digital bandwidth warping matters", xlabel="Live Width (%)",
                   ylabel="Held-out gain-normalized RMSE (dB)")
    axes[1, 1].legend()

    law = result["recommended_law"]
    grid = np.linspace(10, 200, 300)
    axes[2, 0].plot(grid, law["q_at_width_100"] / (grid / 100), color="#333333", label="Q = A / (Width / 100)")
    axes[2, 0].scatter(WIDTHS, [law["individual_fits"][str(w)]["q"] for w in WIDTHS], color="#4477aa", label="Individual fits")
    axes[2, 0].set(title=f"Practical width law: A = {law['q_at_width_100']:.4f}",
                   xlabel="Live Width (%)", ylabel="Butterworth prototype Q")
    axes[2, 0].legend()
    axes[2, 1].plot(grid, law["gain_at_width_100_db"] - 10 * np.log10(grid / 100), color="#333333", label="Gain ∝ 1 / √Width")
    axes[2, 1].scatter(WIDTHS, [law["individual_fits"][str(w)]["gain_db"] for w in WIDTHS], color="#4477aa", label="Individual fits")
    axes[2, 1].set(title="Constant post-sum gain; no AGC in this model", xlabel="Live Width (%)",
                   ylabel="Amplitude gain (dB)")
    axes[2, 1].legend()
    for axis in axes.flat:
        axis.grid(True, alpha=0.2)
    fig.suptitle("Empirical filterbank fit • Live 40 bands, 30–18kHz, Precise, Depth 0, Enhance off\n"
                 "Two Butterworth sections per band; adjacent outputs alternate polarity. This does not identify Ableton's internals.", fontsize=12)
    fig.savefig(destination, dpi=150)
    plt.close(fig)


def write_note(path: Path, result: dict) -> None:
    law = result["recommended_law"]
    verification = result["coefficient_verification"]
    lines = [
        "# Empirical summed-filterbank fit", "",
        "This is a practical model of saved Live H1 measurements, not identification of Ableton's implementation. The data are 48kHz, 40 geometric centers from 30Hz to 18kHz, Precise, Depth0, Enhance off, Modulator carrier. Envelope behavior and Enhance are outside this fit.", "",
        "Candidates include 1–4 identical biquad cascades and Butterworth bandpasses of 2–4 SOS stages (one Butterworth stage equals one ordinary bandpass). Every topology is evaluated with equal or alternating adjacent-band polarity and with or without digital bandwidth compensation.", "",
        "Fit bands have even nearest-center indices within 500–8000Hz; odd indices are held out. Errors weight FFT bins by 1/f, approximately equal weight per log-frequency interval. Each candidate fits Q and a constant gain separately for each Width. The final law fits Width25–200 and is also evaluated at Width10. All data still come from one capture per Width, so held-out bands are frequency validation, not independent recordings. The 2.93Hz FFT resolution and finite settling limit low-frequency/narrow-band accuracy.", "",
        "## Practical model", "",
        f"Use two non-identical SOS sections per band from a second-order Butterworth lowpass prototype transformed to bandpass. Sum bands with alternating signs. For width fraction w = Width/100, Q = {law['q_at_width_100']:.7f}/w and total post-sum gain = {law['gain_at_width_100_linear']:.7f}/sqrt(w). Every band has unity gain at its center before the sum.", "",
        f"A possible band-count extension is Q = {law['q_times_log_band_spacing']:.7f}/(log(maxHz/minHz)/(bands-1) * w). That extension is an inference; only the 40-band setting was fitted. The gain law is likewise verified only at 40 bands.", "",
        "For each center fc: omega = 2*pi*fc/fs; qEff = Q*sin(omega)/omega; u = tan(omega/2); B = u/qEff; uLow = 2*u*u/(sqrt(B*B+4*u*u)+B); uHigh = uLow+B. Convert cutoffs to Hz with fs/pi*atan(uLow/uHigh), then scipy.signal.butter(2, [fLow,fHigh], btype='bandpass', fs=fs, output='sos'). `butterworth_sos()` is the executable recipe; the generated JSON includes an example coefficient pair.", "",
        "For a C++ implementation, map each Butterworth prototype pole p through s² - B*p*s + u² = 0, apply z=(1+s)/(1-s), pair conjugate poles, place two zeros at z=+1 and two at z=-1 across the sections, and normalize the complete cascade to unity at fc. Compute coefficients in double precision and use stable SOS processing. The two section frequencies and Qs differ; duplicating an RBJ section does not give this Butterworth response.", "",
        "The substitution s→(s²+ω₀²)/(s*BW) is documented by [SciPy lp2bp_zpk](" + SOURCES["lowpass_to_bandpass"] + "). The digital transform and absence of automatic prewarping are documented by [SciPy bilinear_zpk](" + SOURCES["bilinear_transform"] + "). The sin(omega)/omega correction and Width/gain laws here are empirical modeling choices validated against the captured data.", "",
        "## Individual full-midband fits", "",
        "| Width % | Prototype Q | Post-sum gain | Shape RMSE dB |", "|---:|---:|---:|---:|",
    ]
    for width in WIDTHS:
        row = law["individual_fits"][str(width)]
        lines.append(f"| {width} | {row['q']:.4f} | {row['gain_linear']:.6f} | {row['shape_rmse_db']:.4f} |")
    lines.extend(["", "## Stability and ringing", "",
        "All tested SOS pole radii remain below one after float32 coefficient quantization. This checks coefficient stability, not runtime rounding noise, denormals, coefficient-update transients, CPU cost, or headroom. Narrow bands necessarily ring longer; steeper skirts do not mean faster settling.", "",
        f"At Width10 the slowest 30Hz pole has radius {verification['widths']['10']['max_pole_radius_float64']:.9f}, with an asymptotic 60dB decay proxy of {verification['widths']['10']['slowest_pole_60db_decay_seconds']:.2f}s. At Width100 it is {verification['widths']['100']['slowest_pole_60db_decay_seconds']:.2f}s. Cascading two sections adds state and filter phase; keep coefficient updates smooth and validate impulse tails and CPU use in the native renderer.", "",
        "The saved complex H1 is also checked against the selected model after fitting a global delay and constant phase. Agreement supports the practical model but does not establish internal structure. No envelope-bank topology, alternate band count, sample rate, or dynamic behavior is inferred from Depth0 measurements.", "",
        "Reproduce: `PYTHONPATH=/tmp/vocoder-compare-libs python3 tools/live_probe/fit_filterbank.py --comparison <source-comparison-root> --output-dir <new-comparison-root>/analysis`", "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comparison", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path,
                        help="Write fit artifacts here, keeping a reference comparison read-only")
    args = parser.parse_args()
    root = args.comparison.resolve()
    source = root / "analysis" / "width_spectra.npz"
    saved = np.load(source)
    frequency = saved["live_25_frequency"]
    mask = (frequency >= 100) & (frequency <= 10000)
    frequency = frequency[mask]
    h1 = {width: saved[f"live_{width}_h1"][mask] for width in WIDTHS}
    measured = {width: 20 * np.log10(np.maximum(np.abs(h1[width]), 1e-15)) for width in WIDTHS}
    nearest = np.clip(np.rint(np.log(frequency / CENTERS[0]) / SPACING), 0, 39).astype(int)
    main_mask = (frequency >= 500) & (frequency <= 8000)
    train, held = main_mask & (nearest % 2 == 0), main_mask & (nearest % 2 == 1)
    low, high = frequency < 500, frequency > 8000
    candidates = [Candidate(family, stages, alternating, compensated)
                  for family in ("cascade", "butterworth")
                  for stages in range(1 if family == "cascade" else 2, 5)
                  for alternating in (False, True) for compensated in (False, True)]
    rows = []
    for candidate in candidates:
        row = fit_candidate(candidate, frequency, measured, train, held, low, high)
        rows.append(row)
        print(f"{candidate.name}: held-out narrow/mid {row['narrow_mid_held_out_rmse_db']:.4f}dB", flush=True)
    rows.sort(key=lambda row: row["narrow_mid_held_out_rmse_db"])
    law, spectra = fit_law(frequency, measured, h1, train, held)
    result = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "source": str(source), "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "sample_rate": FS, "centers_hz": CENTERS.tolist(), "widths": list(WIDTHS),
        "method": {"main_range_hz": [500, 8000], "train": "Even nearest-center indices",
                   "held_out": "Odd nearest-center indices", "weighting": "1/f per FFT bin",
                   "fit": "Constant Q and gain per width; no center shifts; 180-point log-Q grid plus four refined local minima",
                   "limitations": "One recording per width; finite H1 resolution/settling; measured synthesis bank only at Depth0; no internal-topology claim"},
        "selected_candidate": SELECTED.name, "candidates": rows, "recommended_law": law,
        "coefficient_verification": verify_recipe(law["q_at_width_100"]), "sources": SOURCES,
    }
    directory = args.output_dir.resolve() if args.output_dir else root / "analysis"
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "filterbank_fit.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    np.savez_compressed(directory / "filterbank_fit_spectra.npz", **spectra)
    plot(directory / "filterbank_fit.png", result, spectra)
    write_note(directory / "filterbank_fit_notes.md", result)
    print(json.dumps({"selected": SELECTED.name, "q_at_100": law["q_at_width_100"],
                      "gain_at_100": law["gain_at_width_100_linear"], "output": str(directory)}, indent=2))


if __name__ == "__main__":
    main()
