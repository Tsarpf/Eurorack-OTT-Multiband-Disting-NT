#!/usr/bin/env python3
"""Compare measured Live and current native vocoder width responses.

Requires NumPy, SciPy, SoundFile and Matplotlib. This script only reads completed
Live captures; it never sends commands to Live. Optionally regenerate the native
grid with --render-native after building `make -C vocoder render-probe`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy import ndimage, signal


SAMPLE_RATE = 48000
NPERSEG = 16384
START_SECONDS = 1.25
END_SECONDS = 3.75
LOW_HZ = 100
HIGH_HZ = 10000
NATIVE_WIDTHS = [0, 10, 25, 40, 50, 60, 70, 75, 80, 85, 90, 100]


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_audio(path: Path) -> np.ndarray:
    samples, sample_rate = sf.read(path, dtype="float64", always_2d=True)
    if sample_rate != SAMPLE_RATE:
        raise ValueError(f"Expected {SAMPLE_RATE} Hz: {path} is {sample_rate} Hz")
    if samples.shape[1] not in (1, 2) or not np.isfinite(samples).all():
        raise ValueError(f"Unsupported channel count or non-finite samples: {path}")
    return samples


def render_native(root: Path, renderer: Path, widths: list[int]) -> None:
    directory = root / "native" / "width"
    directory.mkdir(parents=True, exist_ok=True)
    for width in widths:
        stem = directory / f"width-{width:03d}"
        subprocess.run([
            str(renderer), "--input", str(root / "inputs" / "white.wav"),
            "--output", str(stem.with_suffix(".wav")),
            "--metadata", str(stem.with_suffix(".json")), "--bands", "40",
            "--width", str(width), "--depth", "0", "--formant-semitones", "0",
            "--min-hz", "30", "--max-hz", "18000", "--attack-ms", "10",
            "--release-ms", "30", "--wet", "100", "--pregain-db", "0",
            "--settle-seconds", "1", "--tail-seconds", "1.5",
        ], check=True, stdout=subprocess.DEVNULL)


def transfer(input_audio: np.ndarray, output_path: Path) -> dict:
    output_audio = read_audio(output_path)
    x, y = input_audio[:, 0], output_audio[:, 0]
    # A broadband correlation estimates the route's bulk delay. The resulting
    # H1 retains the filter's residual phase; only the global delay is removed.
    correlation = signal.correlate(y, x, mode="full", method="fft")
    lags = signal.correlation_lags(len(y), len(x), mode="full")
    valid = (lags >= 0) & (lags <= 3 * SAMPLE_RATE)
    lag = int(lags[valid][np.argmax(np.abs(correlation[valid]))])
    start, stop = round(START_SECONDS * SAMPLE_RATE), round(END_SECONDS * SAMPLE_RATE)
    if lag + stop > len(y) or stop > len(x):
        raise ValueError(f"Aligned steady-state window exceeds recording: {output_path}")
    xs, ys = x[start:stop], y[lag + start:lag + stop]
    welch = dict(fs=SAMPLE_RATE, window="hann", nperseg=NPERSEG,
                 noverlap=NPERSEG // 2, detrend=False, scaling="density", average="mean")
    frequency, pxx = signal.welch(xs, **welch)
    _, pyy = signal.welch(ys, **welch)
    _, pxy = signal.csd(xs, ys, **welch)
    h1 = pxy / np.maximum(pxx, np.finfo(float).tiny)
    coherence = np.clip(np.abs(pxy) ** 2 / np.maximum(pxx * pyy, np.finfo(float).tiny), 0, 1)
    magnitude_db = 20 * np.log10(np.maximum(np.abs(h1), 1e-15))
    mask = (frequency >= LOW_HZ) & (frequency <= HIGH_HZ)
    log_frequency = np.geomspace(LOW_HZ, HIGH_HZ, 2048)
    log_magnitude = np.interp(log_frequency, frequency, magnitude_db)
    octave_step = np.log2(log_frequency[1] / log_frequency[0])
    # Remove a broad spectral trend before measuring band-to-band ripple.
    smooth = ndimage.gaussian_filter1d(log_magnitude, sigma=(1 / 6) / octave_step, mode="nearest")
    ripple = log_magnitude - smooth
    stats = {
        "output": str(output_path), "output_sha256": sha256(output_path),
        "alignment_lag_samples": lag, "alignment_lag_seconds": lag / SAMPLE_RATE,
        "coherence_median": float(np.median(coherence[mask])),
        "coherence_p05": float(np.quantile(coherence[mask], 0.05)),
        "mean_gain_db_linear_hz": float(np.mean(magnitude_db[mask])),
        "mean_gain_db_log_hz": float(np.mean(log_magnitude)),
        "ripple_rms_db": float(np.sqrt(np.mean(ripple ** 2))),
        "ripple_p95_p05_db": float(np.quantile(ripple, 0.95) - np.quantile(ripple, 0.05)),
        "magnitude_p95_p05_db": float(np.quantile(log_magnitude, 0.95) - np.quantile(log_magnitude, 0.05)),
        "steady_output_rms": float(np.sqrt(np.mean(ys ** 2))),
        "stereo_max_abs_difference": float(np.max(np.abs(output_audio[:, 0] - output_audio[:, -1]))),
    }
    return {"frequency": frequency, "h1": h1, "coherence": coherence,
            "magnitude_db": magnitude_db, "mask": mask,
            "log_frequency": log_frequency, "log_magnitude_db": log_magnitude,
            "aligned_output": ys, "stats": stats}


def distance(reference: np.ndarray, native: np.ndarray) -> dict:
    delta = native - reference
    gain_offset = float(np.mean(delta))
    return {
        "raw_gain_rmse_db": float(np.sqrt(np.mean(delta ** 2))),
        "gain_normalized_shape_rmse_db": float(np.sqrt(np.mean((delta - gain_offset) ** 2))),
        "native_minus_reference_gain_db": gain_offset,
        "gain_to_apply_to_native_db": -gain_offset,
    }


def midband_peak(curve: dict, nominal_hz: float) -> dict:
    frequency, magnitude = curve["frequency"], curve["magnitude_db"]
    bin_hz = float(frequency[1] - frequency[0])
    peaks, _ = signal.find_peaks(magnitude, prominence=0.75)
    peaks = peaks[(frequency[peaks] > 600) & (frequency[peaks] < 1600)]
    index = int(peaks[np.argmin(np.abs(frequency[peaks] - nominal_hz))])
    peak_position = int(np.flatnonzero(peaks == index)[0])
    left_neighbor, right_neighbor = int(peaks[peak_position - 1]), int(peaks[peak_position + 1])
    a, b, c = magnitude[index - 1:index + 2]
    fractional_bin = float(0.5 * (a - c) / (a - 2 * b + c))
    peak_hz = float(frequency[index] + fractional_bin * bin_hz)
    peak_db = float(b - 0.25 * (a - c) * fractional_bin)
    threshold = peak_db - 3
    left = index
    while left > left_neighbor and magnitude[left] > threshold:
        left -= 1
    right = index
    while right < right_neighbor and magnitude[right] > threshold:
        right += 1
    if magnitude[left] > threshold or magnitude[right] > threshold:
        raise ValueError("Selected summed-filterbank peak has no separate -3 dB crossings")
    left_hz = float(np.interp(threshold, magnitude[left:left + 2], frequency[left:left + 2]))
    right_hz = float(np.interp(threshold, magnitude[[right, right - 1]], frequency[[right, right - 1]]))
    bandwidth = right_hz - left_hz
    left_minimum = left_neighbor + int(np.argmin(magnitude[left_neighbor:index + 1]))
    right_minimum = index + int(np.argmin(magnitude[index:right_neighbor + 1]))
    return {
        "nominal_native_center_hz": nominal_hz, "estimated_peak_hz": peak_hz,
        "center_offset_from_native_nominal_hz": peak_hz - nominal_hz,
        "peak_gain_db": peak_db, "left_3db_hz": left_hz, "right_3db_hz": right_hz,
        "bandwidth_3db_hz": bandwidth, "q_estimate": peak_hz / bandwidth,
        "frequency_bin_hz": bin_hz, "bandwidth_in_fft_bins": bandwidth / bin_hz,
        "resolution_note": "Coarse estimate: fewer than 8 bins across -3dB bandwidth" if bandwidth / bin_hz < 8 else "Interpolated finite-window estimate",
        "q_bounds_if_bandwidth_changes_by_one_bin": [peak_hz / (bandwidth + bin_hz), peak_hz / (bandwidth - bin_hz)],
        "left_interband_minimum_hz": float(frequency[left_minimum]),
        "right_interband_minimum_hz": float(frequency[right_minimum]),
        "left_interband_attenuation_db": peak_db - float(magnitude[left_minimum]),
        "right_interband_attenuation_db": peak_db - float(magnitude[right_minimum]),
        "skirt_at_minus6percent_db": peak_db - float(np.interp(peak_hz * 0.94, frequency, magnitude)),
        "skirt_at_plus6percent_db": peak_db - float(np.interp(peak_hz * 1.06, frequency, magnitude)),
        "scope": "Peak of the summed filterbank H1, not an isolated band; Q is approximate and cannot establish filter order by itself",
    }


def midband_plot(destination: Path, live: dict, native: dict, midband: dict) -> None:
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), constrained_layout=True)
    colors = ["#3366aa", "#dd7733", "#339977"]
    for color, live_width, native_width in zip(colors, (10, 25, 50), (0, 25, 50)):
        for family, curves, width, style in (("live", live, live_width, "-"), ("native", native, native_width, "--")):
            if width not in curves:
                continue
            curve = curves[width]
            measurement = midband[family][str(width)]
            x = (curve["frequency"] / measurement["estimated_peak_hz"] - 1) * 100
            mask = (x >= -12) & (x <= 12)
            label = f"{family.title()} {width}% • Q≈{measurement['q_estimate']:.1f}"
            axes[0].plot(x[mask], curve["magnitude_db"][mask] - measurement["peak_gain_db"], style,
                         color=color, label=label, lw=1.5)
            if width == 25:
                axes[1].plot(x[mask], curve["magnitude_db"][mask] - measurement["peak_gain_db"], style,
                             color="#3366aa" if family == "live" else "#dd7733", label=label, lw=2)
    for axis in axes:
        axis.axhline(-3, color="gray", lw=0.8, alpha=0.7)
        axis.set(xlim=(-12, 12), xlabel="Frequency offset from estimated peak (%)", ylabel="Magnitude relative to peak (dB)")
        axis.grid(True, alpha=0.2)
        axis.legend(fontsize=8)
    axes[0].set(ylim=(-60, 1), title="Narrow settings • depth 0%")
    axes[1].set(ylim=(-35, 1), title="Similar -3 dB widths, different off-center attenuation")
    fig.suptitle("Measured summed filterbank peak near 940 Hz • 2.93 Hz FFT bins\n"
                 "Peak-aligned for shape comparison; these are not isolated-band responses", fontsize=12)
    fig.savefig(destination, dpi=180)
    plt.close(fig)


def plots(destination: Path, live: dict, native: dict, metrics: dict) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(3, 2, figsize=(15, 13), constrained_layout=True)
    live_widths, native_widths = sorted(live), sorted(native)
    for index, width in enumerate(live_widths):
        curve = live[width]
        mask = curve["mask"]
        axes[0, 0].semilogx(curve["frequency"][mask], curve["magnitude_db"][mask],
                            label=f"{width:g}%", color=plt.cm.viridis(index / max(1, len(live_widths) - 1)), lw=1)
    axes[0, 0].set_title("Ableton Vocoder • measured width")
    for index, width in enumerate([value for value in (0, 25, 50, 75, 100, 150, 200) if value in native]):
        curve = native[width]
        mask = curve["mask"]
        axes[0, 1].semilogx(curve["frequency"][mask], curve["magnitude_db"][mask], label=f"{width:g}%", lw=1)
    axes[0, 1].set_title("Current native vocoder • measured width")
    for axis in axes[0]:
        axis.set(xlim=(LOW_HZ, HIGH_HZ), ylim=(-70, 10), ylabel="H1 magnitude (dB)")
        axis.legend(ncol=3, fontsize=8)
    best = metrics.get("best_for_reference_100")
    if best:
        best_width = best["native_width_percent"]
        for label, curve in (("Live 100%", live[100]),
                             (f"Native {best_width:g}% • best shape", native[best_width]),
                             ("Native 100%", native[100])):
            mask = curve["mask"]
            axes[1, 0].semilogx(curve["frequency"][mask], curve["magnitude_db"][mask] - np.mean(curve["magnitude_db"][mask]),
                                label=label, lw=1.1)
        axes[1, 0].set(xlim=(LOW_HZ, HIGH_HZ), ylim=(-3, 3), ylabel="Gain-normalized magnitude (dB)",
                       title="Live 100% vs native • equal mean dB gain")
        axes[1, 0].legend(fontsize=8)
        score = next(row for row in metrics["comparisons"] if row["live_width_percent"] == 100)
        rows = score["native_candidates"]
        axes[1, 1].plot(native_widths, [row["linear_hz"]["raw_gain_rmse_db"] for row in rows], "o-", label="Raw gain RMSE")
        axes[1, 1].plot(native_widths, [row["linear_hz"]["gain_normalized_shape_rmse_db"] for row in rows], "o-", label="Shape RMSE")
        plateau = metrics.get("native_85_vs_100_plateau", {})
        if plateau.get("steady_waveform_max_abs_difference") == 0:
            axes[1, 1].axvspan(85, 100, alpha=0.12, color="gray", label="Identical native 85–100% output")
        axes[1, 1].set(xlabel="Native width (%)", ylabel="Error vs Live 100% (dB)", title="Native width fit • 100 Hz–10 kHz")
        axes[1, 1].legend(fontsize=8)
    axes[2, 0].plot(live_widths, [live[width]["stats"]["ripple_rms_db"] for width in live_widths], "o-", label="Live")
    axes[2, 0].plot(native_widths, [native[width]["stats"]["ripple_rms_db"] for width in native_widths], "o-", label="Current native")
    axes[2, 0].set(xlabel="Width (%)", ylabel="Ripple RMS (dB)", title="Band ripple after removing broad spectral trend")
    axes[2, 0].legend(fontsize=8)
    if best:
        for label, curve in (("Live 100%", live[100]),
                             (f"Native {best_width:g}%", native[best_width]),
                             ("Native 100%", native[100])):
            mask = curve["mask"]
            axes[2, 1].semilogx(curve["frequency"][mask], curve["coherence"][mask], label=label, lw=1)
        axes[2, 1].set(xlim=(LOW_HZ, HIGH_HZ), ylim=(0.8, 1.01), ylabel="Magnitude-squared coherence",
                       title="How well a linear response describes this window")
        axes[2, 1].legend(fontsize=8)
    for axis in axes.flat:
        axis.grid(True, which="both", alpha=0.2)
    for axis in (axes[0, 0], axes[0, 1], axes[1, 0], axes[2, 1]):
        axis.set_xlabel("Frequency (Hz)")
    fig.suptitle("Self-modulated vocoder • 40 bands • Depth 0% • Enhance off\n"
                 "48 kHz; 30 Hz–18 kHz bank; input 1.25–3.75 s; H1/Welch 16384 samples", fontsize=15)
    fig.savefig(destination, dpi=180)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comparison", type=Path, required=True, help="Directory containing comparison.json")
    parser.add_argument("--render-native", action="store_true", help="Regenerate all native width WAVs first")
    parser.add_argument("--renderer", type=Path, default=Path(__file__).resolve().parents[2] / "vocoder/build/render_probe")
    parser.add_argument("--allow-incomplete", action="store_true", help="Analyze only completed Live cases")
    args = parser.parse_args()
    root = args.comparison.resolve()
    config = read_json(root / "comparison.json")
    widths = config.get("native_widths", NATIVE_WIDTHS)
    if args.render_native:
        render_native(root, args.renderer.resolve(), widths)
    source_path = root / "inputs" / "white.wav"
    source = read_audio(source_path)
    run = Path(config["runs"]["width"])
    plan = read_json(run / "run.json")
    live, missing = {}, []
    for case in plan["cases"]:
        directory = run / case["directory"]
        metadata_path = directory / "metadata.json"
        if not metadata_path.is_file() or read_json(metadata_path).get("status") != "complete":
            missing.append(case["id"])
            continue
        width = round(float(case["parameters"]["Filter Width"]) * 100, 6)
        live[width] = transfer(source, directory / "output.wav")
        live[width]["stats"]["case_id"] = case["id"]
        metadata = read_json(metadata_path)
        live[width]["stats"]["parameter_readback"] = metadata.get("parameter_readback")
    if missing and not args.allow_incomplete:
        raise SystemExit(f"Live width captures are incomplete: {', '.join(missing)}. Rerun after completion.")
    if not live:
        raise SystemExit("No completed Live width captures are available")
    native = {}
    for width in widths:
        stem = root / "native" / "width" / f"width-{width:03d}"
        native[width] = transfer(source, stem.with_suffix(".wav"))
        native[width]["stats"]["renderer_metadata"] = read_json(stem.with_suffix(".json"))
    comparisons = []
    for live_width, reference in sorted(live.items()):
        candidates = []
        for native_width, curve in sorted(native.items()):
            candidates.append({"native_width_percent": native_width,
                               "linear_hz": distance(reference["magnitude_db"][reference["mask"]], curve["magnitude_db"][curve["mask"]]),
                               "log_hz": distance(reference["log_magnitude_db"], curve["log_magnitude_db"])})
        comparisons.append({"live_width_percent": live_width, "native_candidates": candidates,
                            "best_shape_linear_hz": min(candidates, key=lambda value: value["linear_hz"]["gain_normalized_shape_rmse_db"]),
                            "best_raw_linear_hz": min(candidates, key=lambda value: value["linear_hz"]["raw_gain_rmse_db"]),
                            "best_shape_log_hz": min(candidates, key=lambda value: value["log_hz"]["gain_normalized_shape_rmse_db"])})
    metrics = {
        "created_utc": datetime.now(timezone.utc).isoformat(), "comparison_root": str(root),
        "live_width_run": str(run), "input_sha256": sha256(source_path),
        "method": {"sample_rate": SAMPLE_RATE, "input_time_window_seconds": [START_SECONDS, END_SECONDS],
                   "active_noise_starts_seconds": 0.25, "active_warmup_seconds": 1,
                   "estimator": "H1 = mean(conj(X)*Y)/mean(conj(X)*X)",
                   "alignment": "Integer bulk delay from full-signal absolute cross-correlation; left channel",
                   "nperseg": NPERSEG, "noverlap": NPERSEG // 2, "window": "Hann", "averaging": "arithmetic mean",
                   "welch_segments": 1 + (round((END_SECONDS - START_SECONDS) * SAMPLE_RATE) - NPERSEG) // (NPERSEG // 2),
                   "comparison_range_hz": [LOW_HZ, HIGH_HZ],
                   "primary_weighting": "Equal weight per FFT frequency bin (linear Hz)",
                   "secondary_weighting": "2048 equally spaced log-frequency points",
                   "shape_normalization": "Subtract the mean native-minus-reference dB offset before RMSE",
                   "ripple": "RMS and p95-p05 of log-frequency magnitude minus Gaussian trend, sigma=1/6 octave",
                   "controls": "40 bands; Depth0; formant0;30Hz..18kHz;attack10ms;release30ms;wet100%;pregain0;Live Enhance off,Precise",
                   "scope": "Finite-window response at one white-noise level; does not establish nonlinear or transient equivalence"},
        "missing_live_cases": missing,
        "live_curves": [{"width_percent": width, **curve["stats"]} for width, curve in sorted(live.items())],
        "native_curves": [{"width_percent": width, **curve["stats"]} for width, curve in sorted(native.items())],
        "comparisons": comparisons,
    }
    if 100 in live:
        row = next(value for value in comparisons if value["live_width_percent"] == 100)
        metrics["best_for_reference_100"] = row["best_shape_linear_hz"]
    if 85 in native and 100 in native:
        a, b = native[85], native[100]
        mask = a["mask"]
        metrics["native_85_vs_100_plateau"] = {
            **distance(a["magnitude_db"][mask], b["magnitude_db"][mask]),
            "steady_waveform_max_abs_difference": float(np.max(np.abs(a["aligned_output"] - b["aligned_output"]))),
            "synthesis_q_85": a["stats"]["renderer_metadata"]["final_effective_controls"]["synthesis_q"],
            "synthesis_q_100": b["stats"]["renderer_metadata"]["final_effective_controls"]["synthesis_q"],
            "scope": "Depth0 comparison of native85% and100%; a zero waveform difference indicates a plateau."}
    centers = np.geomspace(30, 18000, 40)
    nominal = float(centers[np.argmin(np.abs(centers - 1000))])
    midband = {"native_center_index_zero_based": int(np.argmin(np.abs(centers - 1000))),
               "nominal_center_hz": nominal,
               "live": {str(width): midband_peak(live[width], nominal) for width in (10, 25, 50) if width in live},
               "native": {str(width): midband_peak(native[width], nominal) for width in (0, 25, 50) if width in native},
               "interpretation": "Compare similar -3dB widths as well as notches/skirts; band summation and phase affect these measurements, so filter order is not uniquely identified."}
    metrics["midband_peak_analysis"] = midband
    directory = root / "analysis"
    directory.mkdir(parents=True, exist_ok=True)
    spectra = {}
    for family, curves in (("live", live), ("native", native)):
        for width, curve in curves.items():
            prefix = f"{family}_{width:g}"
            spectra[prefix + "_frequency"] = curve["frequency"]
            spectra[prefix + "_h1"] = curve["h1"]
            spectra[prefix + "_coherence"] = curve["coherence"]
    np.savez_compressed(directory / "width_spectra.npz", **spectra)
    metrics["spectra_file"] = str(directory / "width_spectra.npz")
    (directory / "width.json").write_text(json.dumps(metrics, indent=2, allow_nan=False) + "\n")
    plots(directory / "width.png", live, native, metrics)
    midband_plot(directory / "width_midband.png", live, native, midband)
    print(json.dumps({"best_for_reference_100": metrics.get("best_for_reference_100"),
                      "plateau": metrics.get("native_85_vs_100_plateau"),
                      "best_by_live_width": [{"live_width": value["live_width_percent"],
                                             "native_width": value["best_shape_linear_hz"]["native_width_percent"],
                                             **value["best_shape_linear_hz"]["linear_hz"]} for value in comparisons],
                      "metrics": str(directory / "width.json"), "plot": str(directory / "width.png")}, indent=2))


if __name__ == "__main__":
    main()
