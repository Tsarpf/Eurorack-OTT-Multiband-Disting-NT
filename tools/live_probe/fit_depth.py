#!/usr/bin/env python3
"""Fit static Depth-law candidates to saved Live stepped-tone measurements.

This reads analyze_depth.py's JSON and writes a separate fit report/figure.
It neither renders DSP nor calls Live. The measured curve sums multiple bands;
the resulting per-band law is an approximation, not Live's recovered formula.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares


def db(value: np.ndarray) -> np.ndarray:
    return 20 * np.log10(np.maximum(value, 1e-8))


def affine_power(x: np.ndarray, depth: float, pivot: float, power: float) -> np.ndarray:
    return pivot * np.maximum(0, 1 - depth + depth * (x / pivot) ** (1 / power)) ** power


def selected_law(x: np.ndarray, depth: float) -> np.ndarray:
    if depth == 0:
        return np.ones_like(x)
    if depth <= 1:
        return (x + .56 * (1 - depth)) ** (depth ** 1.2)
    return affine_power(x, depth, 5.5, 3.5)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--measurements", type=Path, required=True,
                        help="Existing depth-comparison.json from analyze_depth.py")
    parser.add_argument("--output", type=Path, required=True,
                        help="Separate JSON output; matching PNG is written beside it")
    args = parser.parse_args()
    if args.output.resolve() == args.measurements.resolve():
        parser.error("The fit report must not replace the source measurements")
    measurements = json.loads(args.measurements.read_text(encoding="utf-8"))
    rows = {(item["enhance"], item["depth_percent"]): item for item in measurements["live"]}
    input_peak_db = np.array([p["input_peak_dbfs"] for p in rows[(False, 0)]["plateaus"][:4]])
    amplitude = 10 ** (input_peak_db / 20)
    gain = {}
    for enhance in (False, True):
        base = np.array([p["output_rms_dbfs"] for p in rows[(enhance, 0)]["plateaus"][:4]])
        for depth in (0, 50, 100, 150, 200):
            output = np.array([p["output_rms_dbfs"] for p in rows[(enhance, depth)]["plateaus"][:4]])
            gain[(enhance, depth)] = output - base
    # The lowest three Depth-100 points give a nearly exact amplitude-linear
    # response; the loudest point bends down by about 3 dB and is not used to
    # pretend that the classic envelope has a different amplitude exponent.
    reference_db = float(np.mean(input_peak_db[:3] - gain[(False, 100)][:3]))
    reference = 10 ** (reference_db / 20)
    x = amplitude / reference
    target = np.array([gain[(False, depth)] for depth in (150, 200)])
    censored = target < -120

    def residual(predicted: np.ndarray) -> np.ndarray:
        error = predicted - target
        # The quietest Depth-200 output is effectively silent. Treat it as a
        # bound, not a fictitious exact -300 dBFS measurement to fit against.
        error[censored] = np.maximum(predicted[censored] + 90, 0)
        return error.ravel()

    def power_prediction(parameters: np.ndarray) -> np.ndarray:
        pivot, expansion = np.exp(parameters)
        return np.array([db(x * (x / pivot) ** (expansion * (depth - 1)))
                         for depth in (1.5, 2)])

    def affine_prediction(parameters: np.ndarray) -> np.ndarray:
        pivot, power = np.exp(parameters)
        return np.array([db(affine_power(x, depth, pivot, power)) for depth in (1.5, 2)])

    power_fit = least_squares(lambda p: residual(power_prediction(p)), np.log([4., 2.]))
    affine_fit = least_squares(lambda p: residual(affine_prediction(p)), np.log([4., 3.]))
    selected_prediction = np.array([db(selected_law(x, depth)) for depth in (1.5, 2)])
    below_fit = least_squares(
        lambda p: db((x + .5 * np.exp(p[0])) ** (.5 ** np.exp(p[1]))) - gain[(False, 50)],
        np.log([.5, 1.2]),
    )
    selected_rows = []
    for enhance in (False, True):
        for depth in (0, 50, 100, 150, 200):
            prediction = db(selected_law(x, depth / 100))
            actual = gain[(enhance, depth)]
            selected_rows.append({
                "enhance": enhance, "depth_percent": depth,
                "measured_gain_relative_to_depth_zero_db": [float(v) if v > -120 else None for v in actual],
                "predicted_gain_relative_to_depth_zero_db": [float(v) if v > -120 else None for v in prediction],
                "errors_db": [float(a - b) if b > -120 and a > -120 else None
                              for a, b in zip(prediction, actual)],
            })
    report = {
        "source_measurements": str(args.measurements.resolve()),
        "source_input_sha256": measurements["input_sha256"],
        "input_peak_dbfs": input_peak_db.tolist(),
        "equivalent_source_reference_peak": reference,
        "equivalent_source_reference_peak_dbfs": reference_db,
        "reference_voltage_calibration": "referenceVolts = digital_reference_peak * bus_volts_per_full_scale * effective_analysis_gain; the effective gain must be measured for the actual filterbank",
        "above_100_candidates": {
            "pivoted_power": {"pivot": float(np.exp(power_fit.x[0])), "expansion": float(np.exp(power_fit.x[1])),
                              "rms_error_db": float(np.sqrt(np.mean(residual(power_prediction(power_fit.x)) ** 2)))},
            "affine_power_fit": {"pivot": float(np.exp(affine_fit.x[0])), "power": float(np.exp(affine_fit.x[1])),
                                 "rms_error_db": float(np.sqrt(np.mean(residual(affine_prediction(affine_fit.x)) ** 2)))},
            "selected_affine_power": {"pivot": 5.5, "power": 3.5,
                                      "rms_error_db": float(np.sqrt(np.mean(residual(selected_prediction) ** 2)))},
        },
        "below_100": {
            "fitted_offset_coefficient": float(np.exp(below_fit.x[0])),
            "fitted_depth_curve_exponent": float(np.exp(below_fit.x[1])),
            "selected_offset_coefficient": .56, "selected_depth_curve_exponent": 1.2,
            "selected_depth_50_rms_error_db": float(np.sqrt(np.mean((db(selected_law(x, .5)) - gain[(False, 50)]) ** 2))),
        },
        "selected_predictions": selected_rows,
        "notes": [
            "Fits steady-state whole-device gains relative to Depth 0, which largely removes the carrier/Enhance response from this measurement.",
            "This is a useful simple per-band approximation, not evidence for Live's exact internal algorithm.",
            "Only four input levels and five Depth settings were measured at 1 kHz, 40 bands, Width 100%; intermediate knob behavior is interpolated.",
            "The chosen law has no moving average, automatic makeup gain, compressor, or time state; only the external envelope follower sets Attack/Release.",
            "The fit does not identify an absolute reference in volts until the target filterbank's analysis gain and host voltage convention are calibrated.",
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(1, 2, figsize=(12, 5.6), constrained_layout=True)
    grid_db = np.linspace(-48, -4, 300)
    grid_x = 10 ** (grid_db / 20) / reference
    for ax, enhance in zip(axes, (False, True)):
        for depth, color in ((0, "gray"), (50, "#377eb8"), (100, "#d95f02"), (150, "#66a61e"), (200, "#008b8b")):
            predicted = db(selected_law(grid_x, depth / 100))
            ax.plot(grid_db, np.where(predicted > -100, predicted, np.nan), color=color, label=f"Depth {depth}%")
            observed = gain[(enhance, depth)]
            visible = observed > -100
            ax.scatter(input_peak_db[visible], observed[visible], color=color, marker="o")
        ax.set(title=f"Measured Live Enhance {'on' if enhance else 'off'} vs selected static law",
               xlabel="Input tone peak (dBFS)", ylabel="Gain relative to Depth 0 (dB)",
               xlim=(-48, -4), ylim=(-80, 40))
        ax.grid(alpha=.2)
        ax.legend(fontsize=9)
    fig.suptitle("Depth approximation: points are measurements; lines are predictions\nFit uses Enhance off; right panel checks the separate Enhance-on data", fontsize=12)
    fig.savefig(args.output.with_suffix(".png"), dpi=160)
    plt.close(fig)
    print(args.output)
    print(json.dumps({"above_100_candidates": report["above_100_candidates"], "below_100": report["below_100"]}))


if __name__ == "__main__":
    main()
