#!/usr/bin/env python3
"""Compare recorded release-step shapes; makes no Live calls or native renders."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

from analyze_depth import (
    FIRST_STEP, STEP_DURATION, cut, db, envelope, estimate_live_offset, mono,
    native_paths, plateau_metrics, read_json, rms, transient_metrics, write_json,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comparison", type=Path, required=True)
    args = parser.parse_args()
    root = args.comparison.resolve()
    source, sr = mono(root / "inputs/steps.wav")
    input_rms = [rms(cut(source, sr, FIRST_STEP + (index + 1) * STEP_DURATION - .3,
                         FIRST_STEP + (index + 1) * STEP_DURATION)) for index in range(5)]
    run = Path(read_json(root / "comparison.json")["runs"]["release"])
    plan, summary = read_json(run / "run.json"), read_json(run / "summary.json")
    if summary["status"] != "complete" or summary["completed"] != len(plan["cases"]):
        raise ValueError("Release reference run is incomplete")
    results, curves = [], []

    def measure(path: Path, label: str, settings: dict, alignment: dict, guard: float) -> None:
        signal, rate = mono(path)
        if rate != sr:
            raise ValueError(f"Unexpected sample rate: {path}")
        offset = alignment["seconds"]
        plateaus = plateau_metrics(signal, sr, offset, input_rms, guard)
        time, env = envelope(signal, sr)
        relative = time - offset - (FIRST_STEP + 4 * STEP_DURATION)
        deviation = db(env) - plateaus[-1]["output_rms_dbfs"]
        eligible = np.flatnonzero((relative >= .005) & (relative <= 1.1))
        minimum = eligible[np.argmin(deviation[eligible])]
        results.append({
            "label": label, "path": str(path), "settings": settings,
            "alignment": alignment,
            "final_plateau_output_rms_dbfs": plateaus[-1]["output_rms_dbfs"],
            "undershoot_db_below_final_plateau": max(0., -float(deviation[minimum])),
            "undershoot_time_ms": float(relative[minimum] * 1000),
            **transient_metrics(signal, sr, offset, plateaus),
            "deviation_from_final_plateau_db": {
                str(milliseconds): float(np.interp(milliseconds / 1000, relative, deviation))
                for milliseconds in (10, 20, 30, 50, 100, 200, 300, 500, 700)
            },
        })
        curves.append((label, relative, deviation))

    for case in plan["cases"]:
        directory = run / case["directory"]
        metadata = read_json(directory / "metadata.json")
        if metadata["status"] != "complete":
            raise ValueError(f"Incomplete case: {case['id']}")
        values = {item["name"]: item["value"] for item in metadata["parameter_readback"]["parameters"]}
        release = round(10 ** values["Release Time"])
        signal, _ = mono(directory / "output.wav")
        measure(directory / "output.wav", f"Live · release {release} ms · Enhance on",
                {"release_ms": release, "width_percent": values["Filter Width"] * 100,
                 "depth_percent": values["Envelope Depth"] * 100, "enhance": bool(values["Enhance"])},
                estimate_live_offset(signal, sr), .04)
    for width in (50, 100):
        path, metadata_path = native_paths(root, width, 100)
        metadata = read_json(metadata_path)
        measure(path, f"Native · release 30 ms · Width {width}%",
                {"release_ms": 30, "width_percent": width, "depth_percent": 100, "enhance": False},
                {"seconds": metadata["input_start_frame"] / sr,
                 "method": "renderer input_start_frame", "uncertainty_seconds": 0}, 0)
    report = {
        "input": str(root / "inputs/steps.wav"), "reference_run": str(run),
        "event": "1 kHz tone steps from -6 to -30 dBFS peak after 1.25 seconds at the louder level",
        "notes": [
            "Measures the combined effective output response, not an internal envelope-release time constant.",
            "Native time is relative to the exact input transition; Live time is relative to the detected steep output fall, uncertain by about 20 ms.",
            "Undershoot and plotted curves are relative to each recording's own final steady level, avoiding an assumption about calibrated native hardware gain.",
            "Native Enhance is unimplemented; the Live release suite uses Enhance on.",
            "Long native recovery is consistent with other gain-control or smoothing stages, but this probe alone does not establish its cause.",
        ],
        "results": results,
    }
    output = root / "analysis/release-response.json"
    write_json(output, report)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(10, 5.8), constrained_layout=True)
    for label, time, deviation in curves:
        ax.plot(time * 1000, deviation, label=label, lw=1.8,
                linestyle="--" if label.startswith("Native") else "-")
    ax.axhspan(-1, 1, alpha=.08, color="black", label="Within 1 dB of final level")
    ax.set(xlim=(-5, 800), ylim=(-20, 25), xlabel="Time after level step / detected output fall (ms)",
           ylabel="Output relative to its final steady level (dB)",
           title="Effective recovery after a −24 dB level step · Depth 100%\n40 bands · 30 Hz–18 kHz · attack 10 ms; Live alignment uncertainty ≈20 ms")
    ax.legend(fontsize=9)
    ax.grid(alpha=.2)
    fig.savefig(output.with_suffix(".png"), dpi=160)
    plt.close(fig)
    print(output)
    print(output.with_suffix(".png"))


if __name__ == "__main__":
    main()
