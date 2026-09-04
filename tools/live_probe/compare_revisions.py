#!/usr/bin/env python3
"""Plot saved before/after vocoder measurements, optionally making an audition.

Requires NumPy, SciPy, SoundFile and Matplotlib. Makes no Live or renderer calls
and leaves source captures untouched. Both comparisons must use the same probes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import soundfile as sf

try:
    from .analyze_depth import FIRST_STEP, STEP_DURATION, db, envelope, mono
except ImportError:
    from analyze_depth import FIRST_STEP, STEP_DURATION, db, envelope, mono


COLORS = {"live": "#244c73", "before": "#d57a35", "after": "#21896e"}


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def pick(rows: list[dict], **values) -> dict:
    matches = [row for row in rows if all(row.get(key) == value for key, value in values.items())]
    if len(matches) != 1:
        raise ValueError(f"Expected one matching measurement: {values}; found {len(matches)}")
    return matches[0]


def same_width(report: dict, percent: int) -> dict:
    row = pick(report["comparisons"], live_width_percent=percent)
    return pick(row["native_candidates"], native_width_percent=percent)


def release_row(report: dict, live: bool) -> dict:
    rows = [row for row in report["results"]
            if row["label"].startswith("Live" if live else "Native")
            and row["settings"]["release_ms"] == 30
            and row["settings"]["width_percent"] == 100
            and row["settings"]["depth_percent"] == 100]
    if len(rows) != 1:
        raise ValueError("Expected exactly one Release30/Width100/Depth100 measurement")
    return rows[0]


def release_curve(row: dict) -> tuple[np.ndarray, np.ndarray]:
    samples, sample_rate = mono(Path(row["path"]))
    time, rms = envelope(samples, sample_rate)
    relative_ms = 1000 * (time - row["alignment"]["seconds"] - FIRST_STEP - 4 * STEP_DURATION)
    deviation = db(rms) - row["final_plateau_output_rms_dbfs"]
    return relative_ms, deviation


def release_summary(row: dict) -> dict:
    return {key: row[key] for key in (
        "path", "settings", "alignment", "final_plateau_output_rms_dbfs",
        "undershoot_db_below_final_plateau", "undershoot_time_ms",
        "down_step_time_to_within_1db_for_100ms_ms")}


def audition(destination: Path, curves: list[tuple[str, dict]]) -> dict:
    """Match RMS after fades; reduce every segment together if peak exceeds .9."""
    chunks, rows, sample_rate = [], [], None
    for label, row in curves:
        path = Path(row["output"])
        samples, rate = sf.read(path, always_2d=True, dtype="float64")
        if rate != 48000 or (sample_rate is not None and rate != sample_rate):
            raise ValueError(f"Expected common 48000 Hz sample rate: {path}")
        if samples.shape[1] not in (1, 2) or not np.isfinite(samples).all():
            raise ValueError(f"Unsupported channels or non-finite audio: {path}")
        sample_rate = rate
        first = row["alignment_lag_samples"] + round(1.25 * rate)
        last = row["alignment_lag_samples"] + round(3.75 * rate)
        if first < 0 or last > len(samples):
            raise ValueError(f"Aligned audition crop exceeds recording: {path}")
        clip = samples[first:last].copy()
        if clip.shape[1] == 1:
            clip = np.repeat(clip, 2, axis=1)
        raw_rms = float(np.sqrt(np.mean(clip ** 2)))
        fade = np.linspace(0, 1, round(0.01 * rate))[:, None]
        clip[:len(fade)] *= fade
        clip[-len(fade):] *= fade[::-1]
        faded_rms = float(np.sqrt(np.mean(clip ** 2)))
        if faded_rms <= 1e-15:
            raise ValueError(f"Cannot level-match silent clip: {path}")
        gain = 0.1 / faded_rms
        clip *= gain
        chunks.append(clip)
        rows.append({"label": label, "source": str(path), "source_sha256": sha256(path),
                     "alignment_lag_samples": row["alignment_lag_samples"],
                     "crop_start_frame": first, "crop_end_frame": last,
                     "source_crop_rms_dbfs": float(db(raw_rms)),
                     "gain_before_common_attenuation": gain})
    maximum_peak = max(float(np.max(np.abs(clip))) for clip in chunks)
    common_gain = min(1.0, 0.9 / maximum_peak)
    silence = np.zeros((round(0.3 * sample_rate), 2))
    sequence, position = [], 0
    for index, (clip, row) in enumerate(zip(chunks, rows)):
        clip *= common_gain
        row.update({"start_seconds": position / sample_rate,
                    "end_seconds": (position + len(clip)) / sample_rate,
                    "applied_gain_linear": row["gain_before_common_attenuation"] * common_gain,
                    "applied_gain_db": float(db(row["gain_before_common_attenuation"] * common_gain)),
                    "output_rms_dbfs": float(db(np.sqrt(np.mean(clip ** 2)))),
                    "output_peak": float(np.max(np.abs(clip)))})
        sequence.append(clip)
        position += len(clip)
        if index + 1 < len(chunks):
            sequence.append(silence)
            position += len(silence)
    result = np.concatenate(sequence)
    destination.parent.mkdir(parents=True, exist_ok=True)
    sf.write(destination, result, sample_rate, subtype="FLOAT")
    report = {"output": str(destination), "sample_rate": sample_rate, "channels": 2,
              "format": "float32 WAV", "duration_seconds": len(result) / sample_rate,
              "input_time_crop_seconds": [1.25, 3.75], "fade_seconds": 0.01,
              "silence_between_seconds": 0.3, "target_rms_dbfs": -20,
              "common_peak_protection_gain": common_gain,
              "output_sha256": sha256(destination), "segments": rows,
              "note": "Listening comparison only: each faded clip is RMS-matched, followed by common attenuation if needed. No limiting or raw-capture changes. Native Enhance is unimplemented; Live uses Enhance on."}
    destination.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="PNG destination; JSON uses the same stem")
    parser.add_argument("--audition", action="store_true", help="Also make Live/old/new +12st listening WAV")
    args = parser.parse_args()
    before, after = args.before.resolve(), args.after.resolve()
    output = args.output.resolve() if args.output else after / "analysis/revision_comparison.png"
    if (output.suffix.lower() != ".png" or output.is_relative_to(before)
            or (args.audition and after.is_relative_to(before))):
        parser.error("Output must be a PNG outside the before comparison")
    reports = {name: {kind: read_json(root / "analysis" / filename)
                     for kind, filename in (("width", "width.json"), ("formant", "formant.json"),
                                            ("release", "release-response.json"))}
               for name, root in (("before", before), ("after", after))}
    for kind in ("width", "formant"):
        if reports["before"][kind]["input_sha256"] != reports["after"][kind]["input_sha256"]:
            raise ValueError(f"Before and after {kind} probes differ")
    width_rows = {name: same_width(report["width"], 25) for name, report in reports.items()}
    formant_rows = {name: {enhance: pick(report["formant"]["comparisons"], enhance=enhance,
                                      formant_semitones=12, native_width_percent=100)
                          for enhance in ("On", "Off")} for name, report in reports.items()}
    release_rows = {"live": release_row(reports["after"]["release"], True),
                    **{name: release_row(report["release"], False) for name, report in reports.items()}}
    if Path(release_row(reports["before"]["release"], True)["path"]) != Path(release_rows["live"]["path"]):
        raise ValueError("Before and after must share the Live release reference")
    result = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "before": str(before), "after": str(after), "figure": str(output),
        "scope": "Native revisions share parameter settings; Live Enhance is labeled per panel and is unavailable in native. Curves are normalized to compare shape, not calibrated overall loudness.",
        "width_25": {"settings": "40 bands;30Hz–18kHz;Depth0;Live Enhance off;both native Width25",
                     "plot": "700–1250Hz H1, each curve normalized to its own local peak",
                     "error_scope": "100–10000Hz; constant dB gain removed; linear_hz and log_hz weightings",
                     **width_rows},
        "formant_plus12": {"settings": "40 bands;30Hz–18kHz;Depth100;all Width100",
                           "plot": "100–10000Hz output/input PSD ratio, equal log-frequency mean removed",
                           "error_scope": "Saved smoothed output-PSD shape RMSE, constant dB gain removed",
                           **formant_rows},
        "release_30": {"event": reports["after"]["release"]["event"],
                       "plot": "5ms centered RMS relative to each recording's final steady level",
                       "alignment_note": "Native exact input step; Live detected output fall, approximately ±20ms uncertainty. This does not measure absolute latency.",
                       **{name: release_summary(row) for name, row in release_rows.items()}},
    }

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({"font.size": 10, "axes.titlesize": 12})
    fig, axes = plt.subplots(3, 1, figsize=(12, 11), constrained_layout=True)
    width_spectra = {name: np.load(root / "analysis/width_spectra.npz")
                     for name, root in (("before", before), ("after", after))}
    for role, dataset, prefix, label in (
            ("live", "after", "live", "Live · Width 25% · Enhance off"),
            ("before", "before", "native", "Old native · Width 25%"),
            ("after", "after", "native", "New native · Width 25%")):
        saved = width_spectra[dataset]
        frequency = saved[f"{prefix}_25_frequency"]
        magnitude = db(np.abs(saved[f"{prefix}_25_h1"]))
        mask = (frequency >= 700) & (frequency <= 1250)
        axes[0].plot(frequency[mask], magnitude[mask] - np.max(magnitude[mask]),
                     color=COLORS[role], lw=2.4 if role == "live" else 1.8,
                     ls="--" if role == "after" else "-", label=label)
    old_error, new_error = [width_rows[name]["linear_hz"]["gain_normalized_shape_rmse_db"]
                            for name in ("before", "after")]
    axes[0].set(title="A  Width 25%: summed filterbank peaks and skirts",
                xlim=(700, 1250), ylim=(-35, 2), xlabel="Frequency (Hz)", ylabel="Relative to local peak (dB)")
    axes[0].text(.02, .06, f"Shape RMSE over 100Hz–10kHz: {old_error:.2f} → {new_error:.2f} dB",
                 transform=axes[0].transAxes, fontsize=10,
                 bbox={"facecolor": "white", "edgecolor": "none", "alpha": .85})
    axes[0].legend(loc="lower right", fontsize=9)

    formant_spectra = {name: np.load(root / "analysis/formant_spectra.npz")
                       for name, root in (("before", before), ("after", after))}
    for role, dataset, key, label in (
            ("live", "after", "live_on_+12", "Live · Enhance on"),
            ("before", "before", "native_w100_+12", "Old native · Width 100%"),
            ("after", "after", "native_w100_+12", "New native · Width 100%")):
        saved = formant_spectra[dataset]
        frequency = saved["frequency_hz"]
        mask = (frequency >= 100) & (frequency <= 10000)
        gain = saved[key][mask] - saved["input"][mask]
        axes[1].semilogx(frequency[mask], gain - np.mean(gain), color=COLORS[role], lw=1.9, label=label)
    saved = formant_spectra["after"]
    gain = saved["live_off_+12"] - saved["input"]
    axes[1].semilogx(saved["frequency_hz"], gain - np.mean(gain), color=COLORS["live"],
                     lw=1, ls="--", alpha=.65, label="Live · Enhance off")
    for guide in (1000, 3000, 7000):
        axes[1].axvline(guide, color="gray", ls=":", lw=.8)
    axes[1].set(title="B  Formant +12 semitones: spectral emphasis follows the shift",
                xlim=(100, 10000), xlabel="Frequency (Hz)", ylabel="Output/input PSD ratio\nmean removed (dB)",
                xticks=[100, 300, 1000, 3000, 7000, 10000], xticklabels=["100", "300", "1k", "3k", "7k", "10k"])
    axes[1].legend(loc="upper left", fontsize=9, ncol=2)

    for role, label in (("live", "Live · Release 30ms · Enhance on"),
                        ("before", "Old native · Release 30ms"),
                        ("after", "New native · Release 30ms")):
        time, deviation = release_curve(release_rows[role])
        axes[2].plot(time, deviation, color=COLORS[role], lw=1.9, label=label)
    axes[2].axhspan(-1, 1, color="gray", alpha=.12)
    for role, text_position in (("before", (230, -12)), ("after", (135, 5))):
        row = release_rows[role]
        amount = row["undershoot_db_below_final_plateau"]
        amount_label = "<0.01" if amount < 0.005 else f"{amount:.2f}"
        axes[2].annotate(f"{amount_label} dB undershoot",
                         (row["undershoot_time_ms"], -amount), xytext=text_position,
                         color=COLORS[role], arrowprops={"arrowstyle": "->", "color": COLORS[role]})
    axes[2].set(title="C  Release 30ms: recovery after the −24dB input level step",
                xlim=(-5, 750), ylim=(-19, 25), xlabel="Time after input step / detected Live output fall (ms)",
                ylabel="Relative to final steady output (dB)")
    axes[2].text(.98, .07, "Width 100% · Depth 100%\nLive alignment uncertainty ≈20ms",
                 transform=axes[2].transAxes, ha="right", va="bottom", fontsize=9)
    axes[2].legend(loc="upper right", fontsize=9)
    for axis in axes:
        axis.grid(alpha=.2)
        axis.set_axisbelow(True)
    fig.suptitle("Vocoder: measured behavior before and after\n"
                 "Matched native settings · 40 bands · self-modulation · 48kHz · 30Hz–18kHz\n"
                 "Normalized curves; Live Enhance is labeled per panel and is unavailable in native", fontsize=13)
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=180)
    plt.close(fig)

    if args.audition:
        curves = [("Live · Enhance on · Width100 · Formant+12st",
                   pick(reports["after"]["formant"]["live_curves"], enhance="On", formant_semitones=12))]
        curves.extend((f"{'Old' if name == 'before' else 'New'} native · Width100 · Formant+12st",
                       pick(report["formant"]["native_curves"], width_percent=100, formant_semitones=12))
                      for name, report in reports.items())
        result["audition"] = audition(after / "analysis/formant-plus12-live-old-new.wav", curves)
    output.with_suffix(".json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({"figure": str(output), "summary": str(output.with_suffix('.json')),
                      "audition": result.get("audition", {}).get("output")}, indent=2))


if __name__ == "__main__":
    main()
