#!/usr/bin/env python3
"""Capture reproducible probes for comparing the current native vocoder to Live.

Requires NumPy and soundfile to prepare signals. Live must already contain the
40-band Modulator/Precise measurement set. The fixed test range is 30–18000 Hz
to preserve comparability with the original baseline (native now supports20Hz).
"""

from __future__ import annotations

import argparse
import hashlib
import math
from pathlib import Path
import sys

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.live_probe.bridge import atomic_json, local_path, read_json
from tools.live_probe.runner import prepare_run, resume_run, run_batch


def prepare(root: Path, workspace: Path) -> dict:
    import numpy as np
    import soundfile as sf

    if (root / "comparison.json").exists():
        raise ValueError("Comparison already exists; omit --prepare-only to resume it")
    (root / "inputs").mkdir(parents=True, exist_ok=True)
    (root / "manifests").mkdir(exist_ok=True)
    sample_rate = 48000
    rng = np.random.default_rng(1701)

    def save(name: str, samples) -> Path:
        padded = np.r_[np.zeros(sample_rate // 4), samples,
                       np.zeros(sample_rate // 4)]
        destination = root / "inputs" / f"{name}.wav"
        sf.write(destination, np.column_stack([padded, padded]), sample_rate,
                 subtype="PCM_24")
        return destination

    noise = rng.uniform(-1, 1, 4 * sample_rate) * 10 ** (-18 / 20)
    white = save("white", noise)
    levels = [-42, -30, -18, -6, -30]
    segment_frames = int(1.25 * sample_rate)
    time = np.arange(segment_frames * len(levels)) / sample_rate
    steps = save("steps", np.sin(2 * np.pi * 1000 * time) *
                 np.repeat(10 ** (np.array(levels) / 20), segment_frames))
    frequency = np.maximum(np.fft.rfftfreq(len(noise), 1 / sample_rate), 1)
    envelope = (.03 + np.exp(-.5 * (np.log2(frequency / 500) / .16) ** 2) +
                .85 * np.exp(-.5 * (np.log2(frequency / 1500) / .12) ** 2) +
                .5 * np.exp(-.5 * (np.log2(frequency / 3500) / .10) ** 2))
    vowel = np.fft.irfft(np.fft.rfft(noise) * envelope, n=len(noise))
    vowel *= 10 ** (-18 / 20) / np.max(np.abs(vowel))
    vowel_path = save("vowel-noise", vowel)

    fixed = {
        "Device On": "On", "Dry/Wet": 1, "Output": 0,
        "Precise/Retro": "Off", "Enhance": "Off", "Unvoiced Level": 0,
        "Mono/Stereo": "Mono", "Gate Threshold": -60,
        "Lower Filter Band": math.log10(30),
        "Upper Filter Band": {"normalized": 1},
        "Filter Width": 1, "Envelope Depth": 0, "Formant Shift": 0,
        "Attack Time": 1, "Release Time": math.log10(30),
    }
    suites = {
        "width": (white, {"Filter Width": [v / 100 for v in
                   (10, 25, 50, 75, 100, 150, 200)]}, {}),
        "depth": (steps, {"Envelope Depth": [0, .5, 1, 1.5, 2],
                          "Enhance": ["Off", "On"]}, {}),
        "formant": (vowel_path, {"Formant Shift": [-24, -12, 0, 12, 24],
                                 "Enhance": ["Off", "On"]},
                    {"Envelope Depth": 1}),
        "release": (steps, {"Release Time": [math.log10(v) for v in
                                              (10, 30, 100)]},
                    {"Envelope Depth": 1, "Enhance": "On"}),
    }
    manifests = {}
    for name, (wave, sweep, overrides) in suites.items():
        manifest = {
            "version": 1, "name": "Native comparison " + name,
            "notes": "40 bands; common 30 Hz–18 kHz; Precise; identical channels.",
            "device": "Filterbank", "sample_rate": sample_rate,
            "settle_seconds": 1, "preroll_seconds": .25, "tail_seconds": 1.5,
            "parameters": {**fixed, **overrides}, "sweep": sweep,
            "signals": [{"name": name, "kind": "wav", "path": str(wave)}],
        }
        path = root / "manifests" / f"{name}.json"
        atomic_json(path, manifest)
        manifests[name] = str(path)
    repository = Path(__file__).resolve().parents[2]
    source_files = ["vocoder_algo.cpp", "vocoder_dsp.h", "batch_biquad.h",
                    "vocoder_parameters.h", "vocoder_structs.h", "envelope_shape.h"]
    config = {
        "workspace": str(workspace), "root": str(root),
        "manifests": manifests, "runs": {},
        "native_widths": [0, 10, 25, 40, 50, 60, 70, 75, 80, 85, 90, 100, 150, 200],
        "native_source_sha256": {
            name: hashlib.sha256((repository / "vocoder" / name).read_bytes()).hexdigest()
            for name in source_files
        },
    }
    atomic_json(root / "comparison.json", config)
    return config


def capture(root: Path, config: dict, suites: list[str]) -> None:
    bridge = Path(config["workspace"]) / "bridge"
    for name in suites:
        if name in config["runs"]:
            run = Path(config["runs"][name])
            plan = resume_run(run)
        else:
            run, plan = prepare_run(Path(config["manifests"][name]), root / "live-runs")
            config["runs"][name] = str(run)
            atomic_json(root / "comparison.json", config)
        print(name.upper(), run, flush=True)
        summary = run_batch(bridge, run, plan,
                            progress=lambda message: print(message.split(" {")[0], flush=True))
        print(summary, flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=local_path,
                        help="Comparison directory; an existing comparison resumes")
    parser.add_argument("--workspace", type=local_path,
                        help="Live Probe workspace, required for a new comparison")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--suites", nargs="+", choices=["width", "depth", "formant", "release"],
                        default=["width", "depth", "formant", "release"])
    args = parser.parse_args()
    root = args.output.resolve()
    if (root / "comparison.json").exists():
        config = read_json(root / "comparison.json")
        if args.prepare_only:
            parser.error("Comparison already exists; choose a new --output")
    else:
        if args.workspace is None:
            parser.error("A new comparison requires --workspace")
        config = prepare(root, args.workspace.resolve())
    if not args.prepare_only:
        capture(root, config, args.suites)


if __name__ == "__main__":
    main()
