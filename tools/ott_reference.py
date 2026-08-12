#!/usr/bin/env python3
"""Render deterministic black-box measurements through Xfer OTT.

The script deliberately depends only on NumPy and DawDreamer.  It does not
open the plug-in editor or an audio device, so the same suite can be rendered
headlessly on Windows.  Output audio stays as float32 in an NPZ file; summary
measurements and the exact plug-in parameters are written as JSON.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

import dawdreamer as daw
import numpy as np


SAMPLE_RATE = 48_000
BLOCK_SIZE = 64

PARAM_DEPTH = 0
PARAM_TIME = 1
PARAM_INPUT_GAIN = 2
PARAM_OUTPUT_GAIN = 3
PARAM_CLEAN_XOVER = 4
PARAM_LOW_GAIN = 8
PARAM_MID_GAIN = 9
PARAM_HIGH_GAIN = 10
PARAM_BYPASS_UP_LOW = 11
PARAM_BYPASS_UP_MID = 12
PARAM_BYPASS_UP_HIGH = 13
PARAM_BYPASS_DOWN_LOW = 14
PARAM_BYPASS_DOWN_MID = 15
PARAM_BYPASS_DOWN_HIGH = 16

STATIC_FREQUENCIES = (40.0, 500.0, 8_000.0)
STATIC_LEVELS_DB = (-120, -110, -100, -90, -80, -70, -60, -50, -40, -35, -30,
                    -25, -20, -15, -10, -5, 0)
STATIC_DEPTHS = (0, 25, 50, 100)
STATIC_SEGMENT_SECONDS = 2.0
STATIC_MEASURE_SECONDS = 0.5

UPWARD_BYPASS_PARAMETERS = (
    PARAM_BYPASS_UP_LOW,
    PARAM_BYPASS_UP_MID,
    PARAM_BYPASS_UP_HIGH,
)
DOWNWARD_BYPASS_PARAMETERS = (
    PARAM_BYPASS_DOWN_LOW,
    PARAM_BYPASS_DOWN_MID,
    PARAM_BYPASS_DOWN_HIGH,
)
EXTENDED_BAND_PROBES = {
    "low": (40.0, PARAM_LOW_GAIN),
    "mid": (500.0, PARAM_MID_GAIN),
    "high": (8_000.0, PARAM_HIGH_GAIN),
}
EXTENDED_BRANCH_MODES = {
    "both_bypassed": (*UPWARD_BYPASS_PARAMETERS,
                       *DOWNWARD_BYPASS_PARAMETERS),
    "upward_only": DOWNWARD_BYPASS_PARAMETERS,
    "downward_only": UPWARD_BYPASS_PARAMETERS,
    "both_active": (),
}
EXTENDED_TONE_SECONDS = 2.0
EXTENDED_MEASURE_SECONDS = 1.0
EXTENDED_CROSSOVER_FREQUENCIES = (
    20.0, 40.0, 88.0, 160.0, 500.0, 1_000.0,
    2_500.0, 8_000.0, 16_000.0, 20_000.0,
)


def db_to_linear(value_db: float) -> float:
    return 10.0 ** (value_db / 20.0)


def linear_to_db(value: float) -> float:
    return 20.0 * math.log10(max(abs(value), 1.0e-20))


def rms(signal: np.ndarray) -> float:
    return float(np.sqrt(np.mean(np.square(signal, dtype=np.float64))))


def new_plugin(plugin_path: Path, signal: np.ndarray,
               parameters: dict[int, float] | None = None):
    if signal.ndim == 1:
        signal = np.vstack((signal, signal))
    signal = np.ascontiguousarray(signal, dtype=np.float32)

    engine = daw.RenderEngine(SAMPLE_RATE, BLOCK_SIZE)
    source = engine.make_playback_processor("source", signal)
    plugin = engine.make_plugin_processor("xfer_ott", str(plugin_path))
    for index, value in (parameters or {}).items():
        if not plugin.set_parameter(index, value):
            raise RuntimeError(f"Xfer OTT rejected parameter {index}={value}")
    engine.load_graph([(source, []), (plugin, [source.get_name()])])
    return engine, plugin


def render(plugin_path: Path, signal: np.ndarray,
           parameters: dict[int, float] | None = None) -> np.ndarray:
    engine, _ = new_plugin(plugin_path, signal, parameters)
    frames = signal.shape[-1]
    if not engine.render(frames / SAMPLE_RATE):
        raise RuntimeError("DawDreamer render failed")
    output = engine.get_audio()
    if output.shape[-1] != frames:
        raise RuntimeError(f"expected {frames} frames, got {output.shape[-1]}")
    if not np.all(np.isfinite(output)):
        raise RuntimeError("Xfer OTT produced a non-finite sample")
    return np.asarray(output, dtype=np.float32)


def inspect_plugin(plugin_path: Path) -> list[dict[str, Any]]:
    signal = np.zeros((2, BLOCK_SIZE), dtype=np.float32)
    _, plugin = new_plugin(plugin_path, signal)
    return plugin.get_parameters_description()


def branch_parameters(depth: float, mode: str,
                      isolated_gain_parameter: int | None = None
                      ) -> dict[int, float]:
    """Return normalized controls for a fresh branch-isolation render.

    Xfer OTT 1.37 treats zero as active (display text ``--``) and every
    positive value as bypassed.  One is used here so the saved controls are
    unambiguous in other hosts too.
    """
    parameters = {PARAM_DEPTH: depth}
    parameters.update({index: 0.0
                       for index in (*UPWARD_BYPASS_PARAMETERS,
                                     *DOWNWARD_BYPASS_PARAMETERS)})
    parameters.update({index: 1.0 for index in EXTENDED_BRANCH_MODES[mode]})
    if isolated_gain_parameter is not None:
        for index in (PARAM_LOW_GAIN, PARAM_MID_GAIN, PARAM_HIGH_GAIN):
            parameters[index] = (0.5 if index == isolated_gain_parameter
                                 else 0.0)
    return parameters


def render_steady_tone(plugin_path: Path, frequency: float,
                       left_peak_dbfs: float, right_peak_dbfs: float,
                       parameters: dict[int, float]) -> dict[str, Any]:
    """Render one tone through one fresh plug-in instance and measure its tail."""
    frames = int(EXTENDED_TONE_SECONDS * SAMPLE_RATE)
    measure_frames = int(EXTENDED_MEASURE_SECONDS * SAMPLE_RATE)
    phase = 2.0 * math.pi * frequency * np.arange(frames) / SAMPLE_RATE
    wave = np.sin(phase).astype(np.float32)
    left = (db_to_linear(left_peak_dbfs) * wave).astype(np.float32)
    right = (db_to_linear(right_peak_dbfs) * wave).astype(np.float32)
    signal = np.ascontiguousarray(np.vstack((left, right)))
    output = render(plugin_path, signal, parameters)
    window = slice(frames - measure_frames, frames)

    input_rms_dbfs = [linear_to_db(rms(channel[window]))
                      for channel in (left, right)]
    output_rms_dbfs = [linear_to_db(rms(output[channel, window]))
                       for channel in range(2)]
    return {
        "frequency_hz": frequency,
        "left_peak_dbfs": left_peak_dbfs,
        "right_peak_dbfs": right_peak_dbfs,
        "input_rms_dbfs": input_rms_dbfs,
        "output_rms_dbfs": output_rms_dbfs,
        "gain_db": [output_rms_dbfs[channel] - input_rms_dbfs[channel]
                    for channel in range(2)],
        "output_peak_dbfs": [
            linear_to_db(float(np.max(np.abs(output[channel, window]))))
            for channel in range(2)
        ],
        "parameters": {str(index): value
                       for index, value in sorted(parameters.items())},
    }


def inspect_bypass_controls(plugin_path: Path) -> list[dict[str, Any]]:
    """Record Xfer's actual normalized bypass polarity and display text."""
    signal = np.zeros((2, BLOCK_SIZE), dtype=np.float32)
    _, plugin = new_plugin(plugin_path, signal)
    rows = []
    for index in (*UPWARD_BYPASS_PARAMETERS, *DOWNWARD_BYPASS_PARAMETERS):
        fresh_value = plugin.get_parameter(index)
        fresh_text = plugin.get_parameter_text(index)
        probes = []
        for value in (0.0, 1.0e-6, 1.0):
            if not plugin.set_parameter(index, value):
                raise RuntimeError(
                    f"Xfer OTT rejected bypass parameter {index}={value}")
            probes.append({
                "set_value": value,
                "read_value": plugin.get_parameter(index),
                "display_text": plugin.get_parameter_text(index),
            })
        rows.append({
            "index": index,
            "name": plugin.get_parameter_name(index),
            "fresh_value": fresh_value,
            "fresh_display_text": fresh_text,
            "probes": probes,
        })
    return rows


def render_extended_probes(plugin_path: Path) -> dict[str, Any]:
    """Render slow, fresh-instance probes used to fit Xfer's static gain law."""
    isolated_branch_rows = []
    for band, (frequency, gain_parameter) in EXTENDED_BAND_PROBES.items():
        for depth in STATIC_DEPTHS:
            for level_db in STATIC_LEVELS_DB:
                for mode in EXTENDED_BRANCH_MODES:
                    parameters = branch_parameters(
                        depth / 100.0, mode, gain_parameter)
                    row = render_steady_tone(
                        plugin_path, frequency, level_db, level_db,
                        parameters)
                    row.update({
                        "band": band,
                        "depth_percent": depth,
                        "nominal_peak_dbfs": level_db,
                        "branch_mode": mode,
                    })
                    isolated_branch_rows.append(row)

    # Keep all three bands audible here.  Comparing Depth 0 with higher Depths
    # while both directions are bypassed exposes Xfer's built-in band makeup.
    baseline_rows = []
    for frequency in STATIC_FREQUENCIES:
        for depth in STATIC_DEPTHS:
            parameters = branch_parameters(depth / 100.0,
                                           "both_bypassed")
            row = render_steady_tone(
                plugin_path, frequency, -35.0, -35.0, parameters)
            row.update({
                "depth_percent": depth,
                "branch_mode": "both_bypassed",
            })
            baseline_rows.append(row)

    # Mute two bands and retain the selected band at its 0 dB setting.  At
    # Depth 0 this measures only Xfer's crossover magnitude and documents how
    # much adjacent-band leakage remains in the representative static tones.
    individual_band_crossover_rows = []
    for band, (_, gain_parameter) in EXTENDED_BAND_PROBES.items():
        for frequency in EXTENDED_CROSSOVER_FREQUENCIES:
            parameters = branch_parameters(
                0.0, "both_bypassed", gain_parameter)
            row = render_steady_tone(
                plugin_path, frequency, -35.0, -35.0, parameters)
            row.update({
                "band": band,
                "depth_percent": 0,
                "branch_mode": "both_bypassed",
            })
            individual_band_crossover_rows.append(row)

    # Same-phase unequal stereo tones make detector linking directly visible:
    # a fully linked detector produces the same gain in both channels.
    stereo_link_rows = []
    for left_db, right_db in ((-60.0, 0.0), (0.0, -60.0),
                              (-60.0, -20.0), (-20.0, -60.0)):
        for mode in EXTENDED_BRANCH_MODES:
            parameters = branch_parameters(1.0, mode)
            row = render_steady_tone(
                plugin_path, 500.0, left_db, right_db, parameters)
            row.update({
                "depth_percent": 100,
                "branch_mode": mode,
                "left_minus_right_gain_db":
                    row["gain_db"][0] - row["gain_db"][1],
            })
            stereo_link_rows.append(row)

    return {
        "tone_seconds": EXTENDED_TONE_SECONDS,
        "measure_seconds": EXTENDED_MEASURE_SECONDS,
        "fresh_instance_per_tone": True,
        "bypass_controls": inspect_bypass_controls(plugin_path),
        "isolated_branch_curves": isolated_branch_rows,
        "all_band_bypass_baselines": baseline_rows,
        "individual_band_crossover_magnitudes":
            individual_band_crossover_rows,
        "stereo_link_probes": stereo_link_rows,
    }


def stepped_sine(frequency: float) -> tuple[np.ndarray, list[slice]]:
    segment_frames = int(STATIC_SEGMENT_SECONDS * SAMPLE_RATE)
    measure_frames = int(STATIC_MEASURE_SECONDS * SAMPLE_RATE)
    frames = segment_frames * len(STATIC_LEVELS_DB)
    phase = 2.0 * math.pi * frequency * np.arange(frames) / SAMPLE_RATE
    signal = np.sin(phase).astype(np.float32)
    windows: list[slice] = []
    for index, level_db in enumerate(STATIC_LEVELS_DB):
        start = index * segment_frames
        end = start + segment_frames
        signal[start:end] *= db_to_linear(level_db)
        windows.append(slice(end - measure_frames, end))
    return signal, windows


def render_static_curves(plugin_path: Path):
    rows: list[dict[str, float | int]] = []
    audio: dict[str, np.ndarray] = {}
    for frequency in STATIC_FREQUENCIES:
        signal, windows = stepped_sine(frequency)
        input_stereo = np.vstack((signal, signal))
        for depth in STATIC_DEPTHS:
            output = render(plugin_path, input_stereo,
                            {PARAM_DEPTH: depth / 100.0})
            key = f"static_{int(frequency)}Hz_depth{depth}"
            audio[key] = output
            for level_db, window in zip(STATIC_LEVELS_DB, windows):
                input_level = linear_to_db(rms(signal[window]))
                output_level = linear_to_db(rms(output[0, window]))
                rows.append({
                    "frequency_hz": frequency,
                    "depth_percent": depth,
                    "nominal_input_dbfs": level_db,
                    "measured_input_dbfs": input_level,
                    "output_dbfs": output_level,
                    "gain_db": output_level - input_level,
                })
    return rows, audio


def render_crossover_probe(plugin_path: Path):
    frames = 65_536
    # Give Xfer's parameter smoothing time to reach Depth=0 before the impulse.
    # Without this pre-roll the probe measures the Depth transition as well as
    # the crossover.
    pre_roll = 2 * SAMPLE_RATE
    full_input = np.zeros((2, pre_roll + frames), dtype=np.float32)
    full_input[:, pre_roll + 512] = 0.5
    full_output = render(plugin_path, full_input, {PARAM_DEPTH: 0.0})
    impulse = full_input[:, pre_roll:]
    output = full_output[:, pre_roll:]

    fft_input = np.fft.rfft(impulse[0])
    fft_output = np.fft.rfft(output[0])
    response = fft_output / np.where(np.abs(fft_input) > 1.0e-20,
                                     fft_input, 1.0)
    frequencies = np.fft.rfftfreq(frames, 1.0 / SAMPLE_RATE)
    probes: dict[str, dict[str, float]] = {}
    for frequency in (20, 40, 88, 160, 500, 1_000, 2_500, 8_000, 16_000,
                      20_000):
        index = int(np.argmin(np.abs(frequencies - frequency)))
        probes[str(frequency)] = {
            "magnitude_db": linear_to_db(abs(response[index])),
            "phase_radians": float(np.angle(response[index])),
        }
    return probes, {
        "crossover_input": impulse,
        "crossover_output": output,
    }


def render_timing_probe(plugin_path: Path):
    # Four one-second plateaus exercise entry to and recovery from the upward
    # and downward regions without introducing a new detector stimulus type.
    levels_db = (-50.0, -8.0, -50.0, -8.0)
    segment_frames = SAMPLE_RATE
    frames = segment_frames * len(levels_db)
    phase = 2.0 * math.pi * 500.0 * np.arange(frames) / SAMPLE_RATE
    signal = np.sin(phase).astype(np.float32)
    for index, level_db in enumerate(levels_db):
        start = index * segment_frames
        signal[start:start + segment_frames] *= db_to_linear(level_db)
    stereo = np.vstack((signal, signal))
    output = render(plugin_path, stereo, {PARAM_DEPTH: 1.0})
    return {
        "timing_input": stereo,
        "timing_output": output,
        "timing_levels_db": np.asarray(levels_db, dtype=np.float32),
    }


def render_control_probes(plugin_path: Path):
    frames = 2 * SAMPLE_RATE
    phase = 2.0 * math.pi * 500.0 * np.arange(frames) / SAMPLE_RATE
    signal = (db_to_linear(-35.0) * np.sin(phase)).astype(np.float32)
    stereo = np.vstack((signal, signal))
    window = slice(frames - SAMPLE_RATE // 2, frames)
    cases = {
        "depth0_default": {PARAM_DEPTH: 0.0},
        "depth100_default": {PARAM_DEPTH: 1.0},
        "depth0_mid_gain_max": {PARAM_DEPTH: 0.0, PARAM_MID_GAIN: 1.0},
        "depth100_mid_gain_max": {PARAM_DEPTH: 1.0, PARAM_MID_GAIN: 1.0},
    }
    rows = []
    input_db = linear_to_db(rms(signal[window]))
    for name, parameters in cases.items():
        output = render(plugin_path, stereo, parameters)
        output_db = linear_to_db(rms(output[0, window]))
        rows.append({
            "case": name,
            "input_dbfs": input_db,
            "output_dbfs": output_db,
            "gain_db": output_db - input_db,
        })
    return rows


def render_band_isolation_probes(plugin_path: Path):
    frames = 2 * SAMPLE_RATE
    window = slice(frames - SAMPLE_RATE // 2, frames)
    bands = {
        "low": (40.0, PARAM_LOW_GAIN),
        "mid": (500.0, PARAM_MID_GAIN),
        "high": (8_000.0, PARAM_HIGH_GAIN),
    }
    rows = []
    for name, (frequency, solo_parameter) in bands.items():
        phase = 2.0 * math.pi * frequency * np.arange(frames) / SAMPLE_RATE
        signal = (db_to_linear(-35.0) * np.sin(phase)).astype(np.float32)
        stereo = np.vstack((signal, signal))
        parameters = {
            PARAM_DEPTH: 0.0,
            PARAM_LOW_GAIN: 0.0,
            PARAM_MID_GAIN: 0.0,
            PARAM_HIGH_GAIN: 0.0,
            solo_parameter: 0.5,
        }
        output = render(plugin_path, stereo, parameters)
        input_db = linear_to_db(rms(signal[window]))
        output_db = linear_to_db(rms(output[0, window]))
        rows.append({
            "band": name,
            "frequency_hz": frequency,
            "gain_db": output_db - input_db,
        })
    return rows


def run_suite(plugin_path: Path, output_stem: Path,
              extended: bool = False) -> None:
    parameters = inspect_plugin(plugin_path)
    static_rows, static_audio = render_static_curves(plugin_path)
    crossover_rows, crossover_audio = render_crossover_probe(plugin_path)
    timing_audio = render_timing_probe(plugin_path)
    control_rows = render_control_probes(plugin_path)
    band_isolation_rows = render_band_isolation_probes(plugin_path)

    metadata = {
        "sample_rate": SAMPLE_RATE,
        "block_size": BLOCK_SIZE,
        "plugin_path": str(plugin_path),
        "parameters": parameters,
        "static_segment_seconds": STATIC_SEGMENT_SECONDS,
        "static_measure_seconds": STATIC_MEASURE_SECONDS,
        "static_curves": static_rows,
        "crossover_response": crossover_rows,
        "control_probes": control_rows,
        "band_isolation_probes": band_isolation_rows,
    }
    if extended:
        metadata["extended_probes"] = render_extended_probes(plugin_path)
    output_stem.parent.mkdir(parents=True, exist_ok=True)
    output_stem.with_suffix(".json").write_text(
        json.dumps(metadata, indent=2), encoding="utf-8")
    np.savez_compressed(output_stem.with_suffix(".npz"),
                        **static_audio, **crossover_audio, **timing_audio)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plugin", type=Path,
                        help="absolute path to Xfer OTT VST2 or VST3")
    parser.add_argument("--output", type=Path,
                        default=Path("xfer_ott_reference"),
                        help="output stem for .json and .npz files")
    parser.add_argument("--inspect", action="store_true",
                        help="only print the plug-in parameter description")
    parser.add_argument("--extended", action="store_true",
                        help=("also render fresh-instance branch, makeup, and "
                              "stereo-link probes (slow)"))
    args = parser.parse_args()

    plugin = args.plugin.resolve()
    if not plugin.exists():
        parser.error(f"plug-in does not exist: {plugin}")
    if args.inspect:
        print(json.dumps(inspect_plugin(plugin), indent=2))
        return
    run_suite(plugin, args.output, args.extended)
    print(args.output.with_suffix(".json"))
    print(args.output.with_suffix(".npz"))


if __name__ == "__main__":
    main()
