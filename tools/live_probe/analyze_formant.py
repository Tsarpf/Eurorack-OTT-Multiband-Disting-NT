#!/usr/bin/env python3
"""Compare completed Live Vocoder formant captures with the native renderer.

Requires NumPy, SciPy, SoundFile and Matplotlib. This reads existing recordings
and never controls Live. Build `make -C vocoder render-probe`, then use
--render-native to regenerate the native comparisons as well.
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
START_SECONDS, END_SECONDS = 1.25, 3.75
LOW_HZ, HIGH_HZ = 100, 10000
SHIFTS = [-24, -12, 0, 12, 24]
LOG_FREQUENCY = np.geomspace(LOW_HZ, HIGH_HZ, 2048)
OCTAVE_STEP = np.log2(LOG_FREQUENCY[1] / LOG_FREQUENCY[0])


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_audio(path: Path) -> np.ndarray:
    audio, sample_rate = sf.read(path, dtype="float64", always_2d=True)
    if sample_rate != SAMPLE_RATE or audio.shape[1] not in (1, 2):
        raise ValueError(f"Expected mono/stereo {SAMPLE_RATE} Hz audio: {path}")
    if not np.isfinite(audio).all():
        raise ValueError(f"Non-finite audio: {path}")
    return audio


def native_stem(root: Path, width: int, shift: int) -> Path:
    return root / "native" / "formant" / f"width{width:g}-formant{shift:+d}"


def render_native(root: Path, renderer: Path, widths: list[int]) -> None:
    (root / "native" / "formant").mkdir(parents=True, exist_ok=True)
    for width in widths:
        for shift in SHIFTS:
            stem = native_stem(root, width, shift)
            subprocess.run([
                str(renderer), "--input", str(root / "inputs" / "vowel-noise.wav"),
                "--output", str(stem.with_suffix(".wav")),
                "--metadata", str(stem.with_suffix(".json")), "--bands", "40",
                "--width", str(width), "--depth", "100", "--formant-semitones", str(shift),
                "--min-hz", "30", "--max-hz", "18000", "--attack-ms", "10",
                "--release-ms", "30", "--wet", "100", "--pregain-db", "0",
                "--settle-seconds", "1", "--tail-seconds", "1.5",
            ], check=True, stdout=subprocess.DEVNULL)


def smooth(values: np.ndarray, octave_sigma: float = 1 / 24) -> np.ndarray:
    return ndimage.gaussian_filter1d(values, octave_sigma / OCTAVE_STEP, mode="nearest")


def prominent_peaks(values: np.ndarray) -> list[dict]:
    """Describe broad measured maxima without assigning them formant identities."""
    broad = smooth(values, 1 / 6)
    indices, properties = signal.find_peaks(broad, prominence=1.5, distance=round((1 / 3) / OCTAVE_STEP))
    ranked = sorted(zip(indices, properties["prominences"]), key=lambda item: item[1], reverse=True)[:8]
    return [{"frequency_hz": float(LOG_FREQUENCY[index]), "prominence_db": float(prominence),
             "relative_level_db": float(broad[index] - np.max(broad))}
            for index, prominence in sorted(ranked)]


def spectrum(input_audio: np.ndarray, output_path: Path) -> dict:
    output_audio = read_audio(output_path)
    x, y = input_audio[:, 0], output_audio[:, 0]
    correlation = signal.correlate(y, x, mode="full", method="fft")
    lags = signal.correlation_lags(len(y), len(x), mode="full")
    valid = (lags >= 0) & (lags <= 3 * SAMPLE_RATE)
    lag = int(lags[valid][np.argmax(np.abs(correlation[valid]))])
    start, stop = round(START_SECONDS * SAMPLE_RATE), round(END_SECONDS * SAMPLE_RATE)
    if lag + stop > len(y) or stop > len(x):
        raise ValueError(f"Aligned steady window exceeds audio length: {output_path}")
    xs, ys = x[start:stop], y[lag + start:lag + stop]
    welch = dict(fs=SAMPLE_RATE, window="hann", nperseg=NPERSEG,
                 noverlap=NPERSEG // 2, detrend=False, scaling="density", average="mean")
    frequency, pxx = signal.welch(xs, **welch)
    _, pyy = signal.welch(ys, **welch)
    _, pxy = signal.csd(xs, ys, **welch)
    coherence = np.clip(np.abs(pxy) ** 2 / np.maximum(pxx * pyy, 1e-100), 0, 1)
    input_db = np.interp(LOG_FREQUENCY, frequency, 10 * np.log10(np.maximum(pxx, 1e-100)))
    output_db = np.interp(LOG_FREQUENCY, frequency, 10 * np.log10(np.maximum(pyy, 1e-100)))
    log_db = smooth(output_db)
    mask = (frequency >= LOW_HZ) & (frequency <= HIGH_HZ)
    stats = {
        "output": str(output_path), "output_sha256": sha256(output_path),
        "alignment_lag_samples": lag, "alignment_lag_seconds": lag / SAMPLE_RATE,
        "aligned_window_correlation": float(np.dot(xs, ys) / np.sqrt(np.dot(xs, xs) * np.dot(ys, ys))),
        "coherence_median": float(np.median(coherence[mask])),
        "coherence_p05": float(np.quantile(coherence[mask], .05)),
        "steady_output_rms": float(np.sqrt(np.mean(ys ** 2))),
        "steady_output_rms_dbfs": float(10 * np.log10(np.mean(ys ** 2))),
        "stereo_max_abs_difference": float(np.max(np.abs(output_audio[:, 0] - output_audio[:, -1]))),
        "broad_output_peaks": prominent_peaks(output_db),
        "broad_output_minus_input_peaks": prominent_peaks(output_db - input_db),
    }
    return {"log_psd_db": log_db, "unsmoothed_log_psd_db": output_db,
            "log_input_psd_db": input_db, "coherence": coherence, "stats": stats}


def distance(reference: np.ndarray, native: np.ndarray) -> dict:
    delta = native - reference
    offset = float(np.mean(delta))
    centered = delta - offset
    return {"raw_rmse_db": float(np.sqrt(np.mean(delta ** 2))),
            "gain_normalized_shape_rmse_db": float(np.sqrt(np.mean(centered ** 2))),
            "gain_normalized_shape_mae_db": float(np.mean(np.abs(centered))),
            "native_minus_reference_mean_db": offset}


def change_metrics(curve: dict, baseline: dict) -> dict:
    delta = curve["log_psd_db"] - baseline["log_psd_db"]
    centered = delta - np.mean(delta)
    rms_db = curve["stats"]["steady_output_rms_dbfs"] - baseline["stats"]["steady_output_rms_dbfs"]
    return {"mean_spectral_change_db": float(np.mean(delta)),
            "gain_normalized_change_rms_db": float(np.sqrt(np.mean(centered ** 2))),
            "steady_output_rms_change_db": rms_db,
            "strongest_relative_boost_hz": float(LOG_FREQUENCY[np.argmax(smooth(centered, 1 / 6))]),
            "strongest_relative_cut_hz": float(LOG_FREQUENCY[np.argmin(smooth(centered, 1 / 6))])}


def plots(directory: Path, live: dict, native: dict, best_width: int) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(5, 2, figsize=(15, 17), sharex=True, constrained_layout=True)
    for row, shift in enumerate(SHIFTS):
        for col, enhance in enumerate(("Off", "On")):
            axis = axes[row, col]
            if (enhance, shift) in live:
                curve = live[enhance, shift]["log_psd_db"]
                axis.semilogx(LOG_FREQUENCY, curve - np.mean(curve), color="black", lw=1.7,
                              label=f"Live BW100, Enhance {enhance}")
            for width in sorted(native):
                curve = native[width][shift]["log_psd_db"]
                axis.semilogx(LOG_FREQUENCY, curve - np.mean(curve), lw=1.5 if width == best_width else .8,
                              alpha=1 if width == best_width else .55, label=f"Native W{width:g}")
            axis.set_title(f"Formant {shift:+d} st • Enhance {enhance}")
            axis.set(xlim=(LOW_HZ, HIGH_HZ), ylim=(-45, 50), ylabel="PSD minus mean (dB)")
            axis.grid(True, which="both", alpha=.2)
            if row == 0:
                axis.legend(fontsize=8, ncol=2)
    for axis in axes[-1]:
        axis.set_xlabel("Frequency (Hz)")
    fig.suptitle("Measured self-modulated vocoder output spectra\n"
                 "40 bands; Depth100%; 30 Hz–18 kHz; native has no Enhance; shapes normalized independently", fontsize=15)
    fig.savefig(directory / "formant_spectra.png", dpi=160)
    plt.close(fig)

    fig, axes = plt.subplots(4, 2, figsize=(15, 13), sharex=True, constrained_layout=True)
    for row, shift in enumerate(value for value in SHIFTS if value):
        for col, enhance in enumerate(("Off", "On")):
            axis = axes[row, col]
            if (enhance, shift) in live and (enhance, 0) in live:
                delta = live[enhance, shift]["log_psd_db"] - live[enhance, 0]["log_psd_db"]
                axis.semilogx(LOG_FREQUENCY, delta - np.mean(delta), color="black", lw=1.7,
                              label=f"Live BW100, Enhance {enhance}")
            for width in sorted(native):
                delta = native[width][shift]["log_psd_db"] - native[width][0]["log_psd_db"]
                axis.semilogx(LOG_FREQUENCY, delta - np.mean(delta), lw=1.5 if width == best_width else .8,
                              alpha=1 if width == best_width else .55, label=f"Native W{width:g}")
            axis.axhline(0, color="gray", lw=.7)
            axis.set_title(f"{shift:+d} st relative to 0 st • Enhance {enhance}")
            axis.set(xlim=(LOW_HZ, HIGH_HZ), ylabel="Gain-normalized PSD change (dB)")
            axis.grid(True, which="both", alpha=.2)
            if row == 0:
                axis.legend(fontsize=8, ncol=2)
    for axis in axes[-1]:
        axis.set_xlabel("Frequency (Hz)")
    fig.suptitle("Formant control: change from each processor's own zero setting\n"
                 "This removes the baseline spectrum and a constant dB offset; it does not remove carrier dependence", fontsize=15)
    fig.savefig(directory / "formant_changes.png", dpi=160)
    plt.close(fig)

    fig, axes = plt.subplots(5, 1, figsize=(13, 15), sharex=True, constrained_layout=True)
    for axis, shift in zip(axes, SHIFTS):
        for enhance, linestyle in (("Off", "-"), ("On", "--")):
            if (enhance, shift) in live:
                curve = live[enhance, shift]
                ratio = curve["log_psd_db"] - smooth(curve["log_input_psd_db"])
                axis.semilogx(LOG_FREQUENCY, ratio - np.mean(ratio), linestyle,
                              color="black", lw=1.7, label=f"Live BW100, Enhance {enhance}")
        for width in sorted(set([50, best_width]) & set(native)):
            curve = native[width][shift]
            ratio = curve["log_psd_db"] - smooth(curve["log_input_psd_db"])
            axis.semilogx(LOG_FREQUENCY, ratio - np.mean(ratio), lw=1.2,
                          label=f"Native W{width:g}")
        for expected in np.array([500, 1500, 3500]) * 2 ** (shift / 12):
            if LOW_HZ < expected < HIGH_HZ:
                axis.axvline(expected, color="purple", linestyle=":", alpha=.4)
                axis.text(expected, 1.01, f"{expected:g}", transform=axis.get_xaxis_transform(),
                          color="purple", ha="center", va="bottom", fontsize=8)
        axis.set_title(f"Formant {shift:+d} st", loc="left", pad=18)
        axis.set(xlim=(LOW_HZ, HIGH_HZ), ylabel="Output/input PSD ratio\nminus mean (dB)")
        axis.grid(True, which="both", alpha=.2)
        if shift == SHIFTS[0]:
            axis.legend(fontsize=9, ncol=2)
    axes[-1].set_xlabel("Frequency (Hz)")
    fig.suptitle("Spectral gain reveals the shifted peaks beneath the fixed carrier spectrum\n"
                 "Dotted lines: 500/1500/3500 Hz shifted by the requested semitones.\n"
                 "Measured output/input power ratio; this is not a recovered internal envelope.", fontsize=14)
    fig.savefig(directory / "formant_spectral_gain.png", dpi=160)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comparison", type=Path, required=True)
    parser.add_argument("--render-native", action="store_true")
    parser.add_argument("--renderer", type=Path, default=Path(__file__).resolve().parents[2] / "vocoder/build/render_probe")
    parser.add_argument("--native-widths", type=int, nargs="+", default=[50, 70, 100])
    parser.add_argument("--allow-incomplete", action="store_true")
    args = parser.parse_args()
    if any(width < 0 or width > 200 for width in args.native_widths):
        parser.error("native widths must be integer percentages in 0..200")
    root = args.comparison.resolve()
    config = read_json(root / "comparison.json")
    width_path = root / "analysis" / "width.json"
    width_reference = read_json(width_path).get("best_for_reference_100") if width_path.exists() else None
    best_width = width_reference["native_width_percent"] if width_reference else 70
    widths = sorted(set([*args.native_widths, best_width]))
    if args.render_native:
        render_native(root, args.renderer.resolve(), widths)
    source_path = root / "inputs" / "vowel-noise.wav"
    source = read_audio(source_path)
    live, missing = {}, []
    run = Path(config["runs"]["formant"]) if "formant" in config["runs"] else None
    if run:
        plan = read_json(run / "run.json")
        for case in plan["cases"]:
            directory = run / case["directory"]
            metadata_path = directory / "metadata.json"
            if not metadata_path.is_file() or read_json(metadata_path).get("status") != "complete":
                missing.append(case["id"])
                continue
            metadata = read_json(metadata_path)
            enhance = str(case["parameters"]["Enhance"])
            shift = int(case["parameters"]["Formant Shift"])
            curve = spectrum(source, directory / "output.wav")
            curve["stats"].update({"case_id": case["id"], "parameter_readback": metadata.get("parameter_readback")})
            live[enhance, shift] = curve
    else:
        missing.append("Live formant run has not started")
    if missing and not args.allow_incomplete:
        raise SystemExit(f"Live captures incomplete: {', '.join(missing)}. Rerun after completion.")
    native = {}
    for width in widths:
        native[width] = {}
        for shift in SHIFTS:
            stem = native_stem(root, width, shift)
            curve = spectrum(source, stem.with_suffix(".wav"))
            curve["stats"]["renderer_metadata"] = read_json(stem.with_suffix(".json"))
            native[width][shift] = curve
    comparisons = []
    for (enhance, shift), reference in sorted(live.items()):
        for width in widths:
            curve = native[width][shift]
            row = {"enhance": enhance, "formant_semitones": shift, "native_width_percent": width,
                   "output_shape": distance(reference["log_psd_db"], curve["log_psd_db"]),
                   "output_shape_unsmoothed": distance(reference["unsmoothed_log_psd_db"], curve["unsmoothed_log_psd_db"])}
            if (enhance, 0) in live:
                row["change_from_zero"] = distance(
                    reference["log_psd_db"] - live[enhance, 0]["log_psd_db"],
                    curve["log_psd_db"] - native[width][0]["log_psd_db"])
            comparisons.append(row)
    metrics = {
        "created_utc": datetime.now(timezone.utc).isoformat(), "comparison_root": str(root),
        "live_formant_run": str(run) if run else None, "input_sha256": sha256(source_path),
        "method": {"sample_rate": SAMPLE_RATE, "input_window_seconds": [START_SECONDS, END_SECONDS],
                   "active_noise_start_seconds": .25, "active_warmup_seconds": 1,
                   "alignment": "Maximum absolute full-audio correlation in 0..3 seconds; left channel",
                   "estimator": "Welch output power spectral density, arithmetic average, Hann window",
                   "nperseg": NPERSEG, "noverlap": NPERSEG // 2,
                   "comparison_range_hz": [LOW_HZ, HIGH_HZ], "log_frequency_points": len(LOG_FREQUENCY),
                   "smoothing_gaussian_sigma_octaves": 1 / 24,
                   "weighting": "Equal log-frequency spacing; best constant dB offset removed for shape RMSE",
                   "broad_peak_sigma_octaves": 1 / 6, "broad_peak_minimum_prominence_db": 1.5,
                   "broad_peak_note": "Measured spectral maxima, not identified linguistic formants. Output-minus-input is a PSD ratio, not a recovered internal envelope.",
                   "change_from_zero": "Each shifted output dB PSD minus its own processor/width/Enhance zero-shift PSD",
                   "controls": "40 bands;Depth100%;30Hz..18kHz;attack10ms;release30ms;wet100%;pregain0;Live BW100%,Precise,Mono",
                   "scope": "One stationary vowel-shaped noise at one level. Carrier spectrum, filterbank and envelope behavior all influence the output; not a full parameter fit."},
        "missing_live_cases": missing, "width_fit_reference": width_reference,
        "highlighted_native_width_percent": best_width,
        "live_curves": [{"enhance": enhance, "formant_semitones": shift, **curve["stats"],
                         **({"change_from_zero": change_metrics(curve, live[enhance, 0])} if (enhance, 0) in live else {})}
                        for (enhance, shift), curve in sorted(live.items())],
        "native_curves": [{"width_percent": width, "formant_semitones": shift, **curve["stats"],
                           "change_from_zero": change_metrics(curve, native[width][0])}
                          for width in widths for shift, curve in sorted(native[width].items())],
        "comparisons": comparisons,
        "enhance_comparisons": [{"formant_semitones": shift,
                                 **distance(live["Off", shift]["log_psd_db"], live["On", shift]["log_psd_db"])}
                                for shift in SHIFTS if ("Off", shift) in live and ("On", shift) in live],
    }
    directory = root / "analysis"
    directory.mkdir(parents=True, exist_ok=True)
    arrays = {"frequency_hz": LOG_FREQUENCY}
    arrays.update({f"live_{enhance.lower()}_{shift:+d}": curve["log_psd_db"] for (enhance, shift), curve in live.items()})
    arrays.update({f"native_w{width:g}_{shift:+d}": curve["log_psd_db"] for width in widths for shift, curve in native[width].items()})
    arrays["input"] = next(iter(native[widths[0]].values()))["log_input_psd_db"]
    np.savez_compressed(directory / "formant_spectra.npz", **arrays)
    metrics["spectra_file"] = str(directory / "formant_spectra.npz")
    (directory / "formant.json").write_text(json.dumps(metrics, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    plots(directory, live, native, best_width)
    print(json.dumps({"metrics": str(directory / "formant.json"), "live_cases": len(live),
                      "native_cases": sum(map(len, native.values())), "missing_live_cases": missing,
                      "highlighted_native_width": best_width}, indent=2))


if __name__ == "__main__":
    main()
