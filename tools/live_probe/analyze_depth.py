#!/usr/bin/env python3
"""Render and compare stepped-tone Depth behavior against captured Live audio.

Requires NumPy, SciPy, soundfile and matplotlib. No Live control calls are made.
The expected input has 250 ms silence followed by five 1.25 s, 1 kHz plateaus
at -42/-30/-18/-6/-30 dBFS peak, then 250 ms silence. Native onset is known from
renderer metadata. Live onset is estimated from the final downward level step;
therefore this report does not claim an absolute latency measurement.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np
import soundfile as sf
from scipy.ndimage import uniform_filter1d


DEPTHS = (0, 50, 100, 150, 200)
WIDTHS = (50, 100)
FIRST_STEP = 0.25
STEP_DURATION = 1.25
STEADY_SECONDS = 0.30
LIVE_EDGE_GUARD = 0.04
EXPECTED_PEAK_LEVELS = (-42, -30, -18, -6, -30)


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def mono(path: Path) -> tuple[np.ndarray, int]:
    signal, sr = sf.read(path, always_2d=True)
    if not np.isfinite(signal).all():
        raise ValueError(f"Nonfinite audio in {path}")
    if signal.shape[1] == 2 and not np.allclose(signal[:, 0], signal[:, 1], atol=1e-9, rtol=0):
        raise ValueError(f"The mono comparison unexpectedly has unequal channels: {path}")
    return signal[:, 0], sr


def rms(values: np.ndarray) -> float:
    if not len(values):
        raise ValueError("Empty measurement window")
    return float(np.sqrt(np.mean(values * values)))


def db(value: float | np.ndarray) -> float | np.ndarray:
    result = 20 * np.log10(np.maximum(value, 1e-15))
    return float(result) if np.ndim(result) == 0 else result


def cut(signal: np.ndarray, sr: int, start: float, end: float) -> np.ndarray:
    first, last = round(start * sr), round(end * sr)
    if first < 0 or last > len(signal) or first >= last:
        raise ValueError(f"Window {start:.4f}..{end:.4f}s outside recording")
    return signal[first:last]


def envelope(signal: np.ndarray, sr: int) -> tuple[np.ndarray, np.ndarray]:
    # Five cycles of the 1 kHz probe suppress carrier ripple. Results are sampled
    # every millisecond and represent a centered RMS window.
    squared = uniform_filter1d(signal * signal, size=round(0.005 * sr), mode="constant")
    hop = max(1, round(sr * 0.001))
    return np.arange(0, len(signal), hop) / sr, np.sqrt(np.maximum(squared[::hop], 0))


def estimate_live_offset(signal: np.ndarray, sr: int) -> dict:
    time, env = envelope(signal, sr)
    levels = db(env)
    # The last transition drops the input by 24 dB. It remains visible even
    # where a high Depth suppresses the earlier quiet plateaus completely.
    falling_step = FIRST_STEP + 4 * STEP_DURATION
    delta = levels[10:] - levels[:-10]
    center = (time[10:] + time[:-10]) / 2
    mask = (center >= falling_step + 0.15) & (center <= falling_step + 0.70)
    candidates = np.flatnonzero(mask)
    selected = candidates[np.argmin(delta[candidates])]
    edge_time = float(center[selected])
    return {
        "seconds": edge_time - falling_step,
        "method": "strongest 10 ms dB-envelope fall near the final -6 to -30 dBFS step",
        "detected_fall_seconds": edge_time,
        "fall_db_in_10ms": float(delta[selected]),
        "uncertainty_seconds": 0.02,
        "steady_window_guard_seconds": LIVE_EDGE_GUARD,
    }


def plateau_metrics(signal: np.ndarray, sr: int, offset: float,
                    input_rms: list[float], guard: float) -> list[dict]:
    result = []
    for index, input_level in enumerate(input_rms):
        end = offset + FIRST_STEP + (index + 1) * STEP_DURATION - guard
        start = end - STEADY_SECONDS
        segment = cut(signal, sr, start, end)
        level = rms(segment)
        result.append({
            "index": index, "input_peak_dbfs": EXPECTED_PEAK_LEVELS[index],
            "input_rms_dbfs": db(input_level), "output_rms_dbfs": db(level),
            "gain_db": db(level) - db(input_level),
            "window_start_seconds": start, "window_end_seconds": end,
            "output_rms": level,
            "below_db_reporting_floor": level <= 1e-15,
        })
    return result


def transient_metrics(signal: np.ndarray, sr: int, offset: float,
                      plateaus: list[dict]) -> dict:
    time, env = envelope(signal, sr)
    step_time = FIRST_STEP + 4 * STEP_DURATION
    relative = time - offset - step_time
    levels = db(env)
    target = plateaus[-1]["output_rms_dbfs"]
    stable = np.abs(levels - target) <= 1
    indices = np.flatnonzero((relative >= 0) & (relative < STEP_DURATION - 0.1))
    settled = None
    for index in indices:
        if stable[index:index + 100].all():
            settled = float(relative[index] * 1000)
            break
    return {
        "down_step_time_to_within_1db_for_100ms_ms": settled,
        "down_step_first_100ms_rms_dbfs": db(rms(cut(signal, sr, offset + step_time, offset + step_time + 0.1))),
        "same_input_level_history_difference_db": plateaus[-1]["output_rms_dbfs"] - plateaus[1]["output_rms_dbfs"],
    }


def native_paths(root: Path, width: int, depth: int) -> tuple[Path, Path]:
    stem = root / "native/depth" / f"width-{width:03d}-depth-{depth:03d}"
    return stem.with_suffix(".wav"), stem.with_suffix(".json")


def render_native(root: Path, executable: Path, force: bool = False,
                  depths: tuple[int, ...] = DEPTHS,
                  check_input_scaling: bool = False) -> None:
    # These historical comparisons deliberately measure the Enhance-off bank.
    # Older revision renderers predate the toggle and already render Off.
    help_text = subprocess.check_output([str(executable), "--help"], text=True)
    enhance_args = ["--enhance", "0"] if "--enhance" in help_text else []
    source = root / "inputs/steps.wav"
    input_hash, renderer_hash = sha256(source), sha256(executable)
    manifest_path = root / "native/depth/render-manifest.json"
    if manifest_path.is_file():
        prior = read_json(manifest_path)
        force = force or prior.get("input_sha256") != input_hash or prior.get("renderer_sha256") != renderer_hash
    commands = []
    for width in WIDTHS:
        for depth in depths:
            output, metadata = native_paths(root, width, depth)
            output.parent.mkdir(parents=True, exist_ok=True)
            command = [str(executable), *enhance_args, "--input", str(source), "--output", str(output),
                       "--metadata", str(metadata), "--bands", "40", "--width", str(width),
                       "--depth", str(depth), "--min-hz", "30", "--max-hz", "18000",
                       "--attack-ms", "10", "--release-ms", "30", "--wet", "100",
                       "--pregain-db", "0", "--settle-seconds", "1", "--tail-seconds", "1.5"]
            if force or not (output.is_file() and metadata.is_file()):
                subprocess.run(command, check=True, capture_output=True, text=True)
            commands.append(command)
    if check_input_scaling:
        scaled_input = root / "native/depth/input-steps-times-five.wav"
        original, rate = sf.read(source, always_2d=True)
        if force or not scaled_input.exists():
            sf.write(scaled_input, original * 5, rate, subtype="FLOAT")
        for depth in (100, 200):
            stem = root / "native/depth" / f"width-100-depth-{depth:03d}-input-times-five"
            output, metadata = stem.with_suffix(".wav"), stem.with_suffix(".json")
            command = [str(executable), *enhance_args, "--input", str(scaled_input), "--output", str(output),
                       "--metadata", str(metadata), "--bands", "40", "--width", "100",
                       "--depth", str(depth), "--min-hz", "30", "--max-hz", "18000",
                       "--attack-ms", "10", "--release-ms", "30", "--wet", "100",
                       "--pregain-db", "0", "--settle-seconds", "1", "--tail-seconds", "1.5"]
            if force or not (output.is_file() and metadata.is_file()):
                subprocess.run(command, check=True, capture_output=True, text=True)
            commands.append(command)
    write_json(manifest_path, {
        "input": str(source), "input_sha256": input_hash,
        "renderer": str(executable), "renderer_sha256": renderer_hash,
        "commands": commands,
    })


def analyze(root: Path, *, native_only: bool = False,
            depths: tuple[int, ...] = DEPTHS,
            check_input_scaling: bool = False) -> tuple[dict, dict]:
    source, sr = mono(root / "inputs/steps.wav")
    if sr != 48000:
        raise ValueError("Expected 48000 Hz comparison files")
    input_rms = [rms(cut(source, sr,
                         FIRST_STEP + (index + 1) * STEP_DURATION - STEADY_SECONDS,
                         FIRST_STEP + (index + 1) * STEP_DURATION))
                 for index in range(5)]
    report = {
        "input": str(root / "inputs/steps.wav"), "sample_rate": sr,
        "input_sha256": sha256(root / "inputs/steps.wav"),
        "input_rms_dbfs": [db(item) for item in input_rms],
        "steady_window_seconds": STEADY_SECONDS,
        "native": [], "live": [], "native_depth_differences": [], "live_depth_differences": [],
        "native_input_scaling": [],
        "notes": [
            "Native measurements use the exact final 300 ms of every plateau.",
            "Live measurements use a 300 ms window ending 40 ms before each estimated transition, to avoid scheduling/filter-delay uncertainty.",
            "Live alignment is suitable for plateau measurements, not absolute latency claims.",
            "Levels retain renderer output units. dB numerical floor is -300 dBFS.",
            "Native Enhance is unimplemented; compare it separately against Live Enhance off/on.",
            "Each native result copies the renderer's bus_volts_per_full_scale. Missing historical metadata means the previous renderer used numeric WAV samples directly as DSP bus values.",
        ],
    }
    audio = {"input": source, "sr": sr, "native": {}, "live": {}}
    for width in WIDTHS:
        for depth in depths:
            output, metadata_path = native_paths(root, width, depth)
            signal, rate = mono(output)
            metadata = read_json(metadata_path)
            if rate != sr or metadata["parameters"]["width_percent"] != width or metadata["parameters"]["depth_percent"] != depth:
                raise ValueError(f"Unexpected native settings/audio: {output}")
            offset = metadata["input_start_frame"] / sr
            plateaus = plateau_metrics(signal, sr, offset, input_rms, 0)
            item = {"width_percent": width, "depth_percent": depth, "path": str(output),
                    "bus_volts_per_full_scale": metadata.get("bus_volts_per_full_scale", metadata["parameters"].get("bus_volts_per_full_scale")),
                    "alignment": {"seconds": offset, "method": "renderer input_start_frame", "uncertainty_seconds": 0},
                    "plateaus": plateaus, "transients": transient_metrics(signal, sr, offset, plateaus)}
            report["native"].append(item)
            audio["native"][(width, depth)] = (signal, offset)
        baseline = next(item for item in report["native"] if item["width_percent"] == width and item["depth_percent"] == 100)
        base_signal, offset = audio["native"][(width, 100)]
        for depth in (item for item in depths if item > 100):
            candidate = next(item for item in report["native"] if item["width_percent"] == width and item["depth_percent"] == depth)
            signal, _ = audio["native"][(width, depth)]
            steady_differences = [
                None if a["below_db_reporting_floor"] or b["below_db_reporting_floor"]
                else a["output_rms_dbfs"] - b["output_rms_dbfs"]
                for a, b in zip(candidate["plateaus"], baseline["plateaus"])
            ]
            jump_differences = []
            for index in range(5):
                start = offset + FIRST_STEP + index * STEP_DURATION
                s = cut(signal, sr, start, start + 0.1)
                b = cut(base_signal, sr, start, start + 0.1)
                jump_differences.append({"step_index": index, "rms_gain_difference_db": db(rms(s)) - db(rms(b)),
                                         "difference_signal_relative_to_100_db": db(rms(s - b)) - db(rms(b))})
            report["native_depth_differences"].append({"width_percent": width, "compared_depth_percent": depth,
                                                       "baseline_depth_percent": 100, "plateau_level_difference_db": steady_differences,
                                                       "first_100ms_after_steps": jump_differences})
    if check_input_scaling:
        scaled_items = []
        for depth in (100, 200):
            stem = root / "native/depth" / f"width-100-depth-{depth:03d}-input-times-five"
            signal, rate = mono(stem.with_suffix(".wav"))
            metadata = read_json(stem.with_suffix(".json"))
            offset = metadata["input_start_frame"] / sr
            normalized = signal / 5
            plateaus = plateau_metrics(normalized, sr, offset, input_rms, 0)
            item = {"depth_percent": depth, "width_percent": 100, "input_multiplier": 5,
                    "bus_volts_per_full_scale": metadata.get("bus_volts_per_full_scale", metadata["parameters"].get("bus_volts_per_full_scale")),
                    "output_multiplier_for_report": 0.2, "path": str(stem.with_suffix(".wav")),
                    "plateaus": plateaus, "transients": transient_metrics(normalized, sr, offset, plateaus)}
            scaled_items.append(item)
        report["native_input_scaling"] = scaled_items
        report["native_scaled_200_vs_100_plateau_difference_db"] = [
            a["output_rms_dbfs"] - b["output_rms_dbfs"]
            for a, b in zip(scaled_items[1]["plateaus"], scaled_items[0]["plateaus"])
        ]
        report["notes"].append("The optional 5x-input stress test divides native output by 5 for reported RMS units; recorded float WAVs remain unscaled. At a 5 V full-scale convention this is a 25 V full-scale input stress test.")
    if native_only:
        return report, audio
    comparison = read_json(root / "comparison.json")
    run_path = comparison.get("runs", {}).get("depth")
    if not run_path:
        raise ValueError("Live Depth run is not published in comparison.json yet; use --native-only or rerun later")
    run = Path(run_path)
    plan = read_json(run / "run.json")
    summary = read_json(run / "summary.json")
    if summary.get("status") != "complete" or summary.get("completed") != len(plan["cases"]):
        raise ValueError("Live Depth run has not completed; do not analyze partial reference captures")
    report["live_run"] = str(run)
    for case in plan["cases"]:
        directory = run / case["directory"]
        metadata = read_json(directory / "metadata.json")
        if metadata.get("status") != "complete":
            raise ValueError(f"Incomplete reference case {case['id']}")
        values = {item["name"]: item["value"] for item in metadata["parameter_readback"]["parameters"]}
        depth = round(values["Envelope Depth"] * 100)
        enhance = bool(values["Enhance"])
        signal, rate = mono(directory / "output.wav")
        if rate != sr:
            raise ValueError("Reference sample rate differs")
        alignment = estimate_live_offset(signal, sr)
        offset = alignment["seconds"]
        plateaus = plateau_metrics(signal, sr, offset, input_rms, LIVE_EDGE_GUARD)
        item = {"depth_percent": depth, "enhance": enhance, "case_id": case["id"],
                "path": str(directory / "output.wav"), "alignment": alignment,
                "plateaus": plateaus, "transients": transient_metrics(signal, sr, offset, plateaus)}
        report["live"].append(item)
        audio["live"][(enhance, depth)] = (signal, offset)
    for enhance in (False, True):
        baseline = next(item for item in report["live"] if item["enhance"] == enhance and item["depth_percent"] == 100)
        candidate = next(item for item in report["live"] if item["enhance"] == enhance and item["depth_percent"] == 200)
        report["live_depth_differences"].append({"enhance": enhance, "baseline_depth_percent": 100, "compared_depth_percent": 200,
                                                "plateau_level_difference_db": [
                                                    None if a["below_db_reporting_floor"] or b["below_db_reporting_floor"]
                                                    else a["output_rms_dbfs"] - b["output_rms_dbfs"]
                                                    for a, b in zip(candidate["plateaus"], baseline["plateaus"])],
                                                "null_difference_means": "One measured plateau is below the -300 dBFS reporting floor; no finite level difference is reported."})
    return report, audio


def plot(report: dict, audio: dict, output: Path) -> None:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    colors = {0: "#808080", 50: "#377eb8", 100: "#d95f02", 150: "#66a61e", 200: "#008b8b", 400: "#7b3294"}
    fig, axes = plt.subplots(2, 3, figsize=(17, 10), constrained_layout=True)
    x = np.array(report["input_rms_dbfs"])
    source_time, source_env = envelope(audio["input"], audio["sr"])
    for column, width in enumerate(WIDTHS):
        ax = axes[0, column]
        for item in report["native"]:
            if item["width_percent"] != width:
                continue
            depth = item["depth_percent"]
            levels = [p["output_rms_dbfs"] for p in item["plateaus"]]
            color = colors.get(depth, "#984ea3")
            ax.plot(x[:4], levels[:4], "o-", color=color, label=f"Depth {depth}%", lw=2 if depth in (100, 200, 400) else 1, alpha=1 if depth in (100, 200, 400) else .6)
            ax.scatter(x[4], levels[4], marker="v", facecolors="none", edgecolors=color)
        ax.plot([-50, 0], [-50, 0], "k--", lw=1, alpha=.5, label="Unity gain")
        ax.set(title=f"Current native · Width {width}%", xlabel="Input RMS (dBFS)", ylabel="Output RMS (dBFS)", ylim=(-110, 5))
        ax.legend(fontsize=8, ncol=2)
        ax = axes[1, column]
        ax.plot(source_time, db(source_env), "k--", lw=1, alpha=.6, label="Input")
        for depth in (100, 200, 400):
            if (width, depth) not in audio["native"]:
                continue
            signal, offset = audio["native"][(width, depth)]
            time, env = envelope(signal, audio["sr"])
            ax.plot(time - offset, db(env), color=colors[depth], lw=1.5, label=f"Depth {depth}%")
        ax.set(title=f"Current native · Width {width}% · level-step response", xlabel="Time relative to input file (s)", ylabel="5 ms RMS envelope (dBFS)", xlim=(.15, 6.8), ylim=(-100, 5))
        ax.legend(fontsize=8)
    ax = axes[0, 2]
    for item in report["live"]:
        if item["depth_percent"] not in (0, 100, 200):
            continue
        depth = item["depth_percent"]
        levels = [p["output_rms_dbfs"] for p in item["plateaus"]]
        visible = np.array(levels[:4]) > -110
        ax.plot(x[:4][visible], np.array(levels[:4])[visible], marker="o", linestyle="-" if item["enhance"] else "--", color=colors[depth], label=f"{depth}% · Enhance {'on' if item['enhance'] else 'off'}")
        if not visible.all():
            ax.scatter(x[:4][~visible], np.full(np.sum(~visible), -108), marker="v", color=colors[depth])
    ax.plot([-50, 0], [-50, 0], "k:", lw=1, alpha=.5)
    ax.set(title="Ableton Live · Width 100% · Precise", xlabel="Input RMS (dBFS)", ylabel="Output RMS (dBFS)", ylim=(-110, 5))
    ax.legend(fontsize=8, ncol=2)
    ax = axes[1, 2]
    ax.plot(source_time, db(source_env), "k:", lw=1, alpha=.6, label="Input")
    for enhance in (False, True):
        for depth in (100, 200):
            if (enhance, depth) not in audio["live"]:
                continue
            signal, offset = audio["live"][(enhance, depth)]
            time, env = envelope(signal, audio["sr"])
            ax.plot(time - offset, db(env), color=colors[depth], linestyle="-" if enhance else "--", lw=1.4, label=f"{depth}% · Enhance {'on' if enhance else 'off'}")
    ax.set(title="Ableton Live · level-step response", xlabel="Input-relative time, approximate alignment (s)", ylabel="5 ms RMS envelope (dBFS)", xlim=(.15, 6.8), ylim=(-110, 5))
    ax.legend(fontsize=8, ncol=2)
    for ax in axes.flat:
        ax.grid(alpha=.2)
    scales = {item["bus_volts_per_full_scale"] for item in report["native"]}
    if len(scales) == 1 and None not in scales:
        units = f"Native renderer uses {next(iter(scales)):g} bus volts per full-scale WAV amplitude; output is in WAV units"
    else:
        units = "Historical numeric bus units or mixed renderer conventions; see per-render metadata"
    fig.suptitle("Depth comparison · 40 bands · 30 Hz–18 kHz · attack 10 ms · release 30 ms\n" + units + "\nHollow triangles repeat −30 dBFS; downward markers at the Live axis floor indicate values below −110 dBFS", fontsize=12)
    output.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output, dpi=160)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--comparison", type=Path, required=True)
    parser.add_argument("--renderer", type=Path, default=Path(__file__).resolve().parents[2] / "vocoder/build/render_probe")
    parser.add_argument("--skip-render", action="store_true")
    parser.add_argument("--force-render", action="store_true")
    parser.add_argument("--depths", type=int, nargs="+", default=list(DEPTHS),
                        help="Native grid, default 0 50 100 150 200; add 400 only with a compatible historical renderer")
    parser.add_argument("--check-input-scaling", action="store_true",
                        help="Also render/analyze 5x louder input; this stresses guards with a renderer that already maps full scale to 5 V")
    parser.add_argument("--native-only", action="store_true", help="Analyze native output before the Live batch finishes")
    args = parser.parse_args()
    if 100 not in args.depths or len(set(args.depths)) != len(args.depths) or any(item < 0 for item in args.depths):
        parser.error("--depths must contain 100, contain no duplicates, and use nonnegative integers")
    depths = tuple(sorted(args.depths))
    root = args.comparison.resolve()
    if not args.skip_render:
        render_native(root, args.renderer.resolve(), args.force_render, depths, args.check_input_scaling)
    report, audio = analyze(root, native_only=args.native_only, depths=depths,
                            check_input_scaling=args.check_input_scaling)
    suffix = "depth-native" if args.native_only else "depth-comparison"
    metrics = root / "analysis" / f"{suffix}.json"
    write_json(metrics, report)
    figure = metrics.with_suffix(".png")
    plot(report, audio, figure)
    print(metrics)
    print(figure)
    for item in report["native_depth_differences"] + report["live_depth_differences"]:
        print(json.dumps(item))


if __name__ == "__main__":
    main()
