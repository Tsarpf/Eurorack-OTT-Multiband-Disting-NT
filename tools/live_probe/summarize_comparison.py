#!/usr/bin/env python3
"""Build a compact overview from existing vocoder comparison analysis files.

Requires NumPy and Matplotlib. Reads saved metrics/spectra only; no Live or DSP
rendering calls are made. Spectra in formant_spectra.npz are already dB PSDs.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comparison", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    analysis = args.comparison.resolve() / "analysis"
    output = args.output or analysis / "comparison_summary.png"
    depth = json.loads((analysis / "depth-comparison.json").read_text())
    width_metrics = json.loads((analysis / "width.json").read_text())
    matched_width = width_metrics["best_for_reference_100"]["native_width_percent"]
    live_depth = next(row for row in depth["live_depth_differences"]
                      if row["enhance"] and row["baseline_depth_percent"] == 100
                      and row["compared_depth_percent"] == 200)
    native_depth = next(row for row in depth["native_depth_differences"]
                        if row["width_percent"] == 100 and row["baseline_depth_percent"] == 100
                        and row["compared_depth_percent"] == 200)
    baseline = next(row for row in depth["native"]
                    if row["width_percent"] == 100 and row["depth_percent"] == 100)
    levels = [-30, -18, -6]
    indices = [next(p["index"] for p in baseline["plateaus"] if p["input_peak_dbfs"] == level)
               for level in levels]
    live_delta = np.asarray([live_depth["plateau_level_difference_db"][i] for i in indices], dtype=float)
    native_delta = np.asarray([native_depth["plateau_level_difference_db"][i] for i in indices], dtype=float)
    if not np.isfinite(live_delta).all() or not np.isfinite(native_delta).all():
        raise ValueError("Requested depth differences are missing or non-finite")

    plt.rcParams.update({"font.size": 10, "axes.titlesize": 12, "axes.labelsize": 10})
    fig, axes = plt.subplots(3, 1, figsize=(12, 11), constrained_layout=True)
    live_color, native_color, alternative_color = "#2764a5", "#d76c26", "#3c9475"
    positions = np.arange(len(levels))
    axes[0].bar(positions, live_delta, width=0.42, color=live_color,
                label="Live · Enhance on · Width 100%")
    axes[0].plot(positions, native_delta, "o--", color=native_color, lw=2,
                 label="Native · Width 100%")
    for position, value in zip(positions, live_delta):
        axes[0].annotate(f"{value:+.1f} dB", (position, value),
                         xytext=(0, -13 if value < 0 else 6), textcoords="offset points",
                         ha="center", va="top" if value < 0 else "bottom", color=live_color, fontweight="bold")
    axes[0].axhline(0, color="gray", lw=0.7, zorder=0)
    axes[0].set(xticks=positions, xticklabels=[str(value) for value in levels],
                xlabel="Input peak level (dBFS)", ylabel="Output RMS change (dB)", ylim=(-44, 16))
    axes[0].set_title("A  Depth 100% → 200%: sustained output changes differently\n"
                      "Stepped sine • late 300 ms of each plateau", loc="left")
    axes[0].legend(loc="lower right", fontsize=9)

    with np.load(analysis / "width_spectra.npz") as width:
        for family, label, color in (("live", "Live 25% · Enhance off", live_color),
                                      ("native", "Native 25%", native_color)):
            frequency = width[f"{family}_25_frequency"]
            magnitude = 20 * np.log10(np.maximum(np.abs(width[f"{family}_25_h1"]), 1e-15))
            mask = (frequency >= 700) & (frequency <= 1250)
            axes[1].plot(frequency[mask], magnitude[mask] - np.max(magnitude[mask]),
                         color=color, lw=1.8, label=label)
    axes[1].set(xlim=(700, 1250), ylim=(-35, 2), xlabel="Frequency (Hz)", ylabel="Relative to local peak (dB)")
    axes[1].set_title("B  Width 25%: measured peak width and inter-band attenuation\n"
                      "White noise • Depth 0% • summed filterbank H1", loc="left")
    axes[1].legend(loc="lower right", fontsize=9)

    with np.load(analysis / "formant_spectra.npz") as formant:
        frequency = formant["frequency_hz"]
        mask = (frequency >= 100) & (frequency <= 10000)
        alternative_width = 50 if matched_width != 50 else 100
        for key, label, color in (("live_on_+12", "Live · Enhance on · Width 100%", live_color),
                                  (f"native_w{matched_width:g}_+12", f"Native · Width {matched_width:g}%", native_color),
                                  (f"native_w{alternative_width:g}_+12", f"Native · Width {alternative_width:g}%", alternative_color)):
            gain_db = formant[key][mask] - formant["input"][mask]
            gain_db -= np.mean(gain_db)  # Equal log-frequency mean, not a level calibration.
            axes[2].semilogx(frequency[mask], gain_db, color=color, lw=1.6, label=label)
    for guide in (1000, 3000, 7000):
        axes[2].axvline(guide, color="gray", lw=0.8, ls=":", zorder=0)
    axes[2].set(xlim=(100, 10000), xlabel="Frequency (Hz)", ylabel="Output/input PSD ratio\nmean removed (dB)",
                xticks=[100, 300, 1000, 3000, 7000, 10000], xticklabels=["100", "300", "1k", "3k", "7k", "10k"])
    axes[2].set_title("C  Formant +12 semitones: different spectral emphasis\n"
                      "Vowel-shaped noise • Depth 100% • each curve's mean removed", loc="left")
    axes[2].legend(loc="lower right", fontsize=9)
    for axis in axes:
        axis.grid(True, alpha=0.2)
        axis.set_axisbelow(True)
    fig.suptitle("Ableton Vocoder vs current native vocoder\n"
                 "40 bands • self-modulated • 48 kHz • common 30 Hz–18 kHz filter range", fontsize=15)
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=180)
    plt.close(fig)
    print(json.dumps({"output": str(output.resolve()), "input_peak_dbfs": levels,
                      "live_depth_200_minus_100_db": live_delta.tolist(),
                      "native_depth_200_minus_100_db": native_delta.tolist()}, indent=2))


if __name__ == "__main__":
    main()
