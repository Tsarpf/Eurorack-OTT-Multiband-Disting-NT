#!/usr/bin/env python3
"""Capture Xfer dynamics offline, or compare a native OTT renderer to a capture.

Capture needs a platform-matching Xfer VST, DawDreamer and NumPy. Comparison
needs only NumPy and the executable built by `make build/ott_render`.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import numpy as np

SR = 48000


def probes():
    """Deterministic fitting and held-out signals; amplitudes are float units."""
    t = np.arange(4 * SR) / SR
    cases = {}
    for hz in (40, 500, 8000):
        x = np.sin(2 * np.pi * hz * t).astype(np.float32)
        x *= np.repeat(10.0 ** (np.array([-50, -8, -50, -8]) / 20), SR).astype(np.float32)
        cases[f"step_{hz}"] = (x, 100, "fit")
    levels = np.repeat([-50., -8., -50., -8.], SR)
    for i in (1, 2, 3):
        levels[i * SR:i * SR + 480] = np.linspace(levels[i * SR - 1], levels[i * SR], 480)
    cases["ramp"] = ((np.sin(2 * np.pi * 500 * t) * 10 ** (levels / 20)).astype(np.float32), 100, "validation")
    t = np.arange(5 * SR) / SR
    x = np.zeros_like(t)
    tone = sum(np.sin(2 * np.pi * 110 * h * t) / h for h in range(1, 21))
    for i in range(8):
        dt = t - .25 - i * .5
        env = np.where(dt >= 0, (1 - np.exp(-np.maximum(dt, 0) / .002)) * np.exp(-np.maximum(dt, 0) / .12), 0)
        x += env * tone
    x = (x * .35 / np.max(np.abs(x))).astype(np.float32)
    cases["plucks"] = (x, 100, "validation")
    for depth in (25, 50):
        cases[f"plucks_depth{depth}"] = (x, depth, "held_out")
    t = np.arange(4 * SR) / SR
    # Different frequency/levels, plus stereo imbalance and changing spectra.
    levels = np.repeat(10.0 ** (np.array([-65., -18., -35., -3.]) / 20), SR)
    x = (np.sin(2 * np.pi * 110 * t) * levels).astype(np.float32)
    cases["step_110"] = (x, 100, "held_out")
    cases["stereo_steps"] = (np.vstack((x, np.roll(x, SR))), 100, "held_out")
    rng = np.random.default_rng(1701)
    noise = rng.standard_normal(len(t))
    env = np.zeros_like(t)
    for start in (.2, .7, 1.4, 2.2, 3.0):
        dt = np.maximum(t - start, 0)
        env += np.where(t >= start, (1 - np.exp(-dt / .001)) * np.exp(-dt / .08), 0)
    noise = (noise * env * .12).astype(np.float32)
    cases["noise_bursts"] = (noise, 100, "held_out")
    return cases


def capture(args):
    import ott_reference as reference
    data = {}
    descriptions = {}
    for name, (x, depth, purpose) in probes().items():
        if x.ndim == 1:
            x = np.vstack((x, x))
        parameters = reference.branch_parameters(depth / 100, "both_active")
        data[name + "_input"] = x
        data[name + "_output"] = reference.render(args.plugin, x, parameters)
        descriptions[name] = dict(depth=depth, purpose=purpose, parameters=parameters)
    if args.branches:
        for band, (hz, gain_param) in reference.EXTENDED_BAND_PROBES.items():
            x = data[f"step_{int(hz)}_input"]
            for mode in reference.EXTENDED_BRANCH_MODES:
                name = f"{band}_{mode}"
                parameters = reference.branch_parameters(1, mode, gain_param)
                data[name + "_input"] = x
                data[name + "_output"] = reference.render(args.plugin, x, parameters)
                descriptions[name] = dict(depth=100, purpose="isolated_fit", parameters=parameters)
    metadata = dict(sample_rate=SR, host_block_size=reference.BLOCK_SIZE,
                    plugin_sha256=hashlib.sha256(args.plugin.read_bytes()).hexdigest(),
                    plugin_parameters=reference.inspect_plugin(args.plugin), cases=descriptions)
    data["metadata"] = np.asarray(json.dumps(metadata))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.output, **data)
    args.output.with_suffix(".json").write_text(json.dumps(metadata, indent=2))


def rms(x):
    return np.sqrt(np.mean(np.square(x, dtype=np.float64)))


def db(x):
    return 20 * np.log10(np.maximum(x, 1e-30))


def compare(args):
    with np.load(args.reference, allow_pickle=False) as ref, tempfile.TemporaryDirectory() as temp:
        metadata = json.loads(str(ref["metadata"]))
        if metadata["sample_rate"] != SR:
            raise ValueError("renderer currently supports 48 kHz captures")
        result = dict(renderer_sha256=hashlib.sha256(args.renderer.read_bytes()).hexdigest(),
                      reference_sha256=hashlib.sha256(args.reference.read_bytes()).hexdigest(),
                      block_size=args.block_size, cases={})
        for name, spec in metadata["cases"].items():
            if spec["purpose"] == "isolated_fit":
                continue  # These captures fit the detector; native run is full-band.
            x, r = ref[name + "_input"], ref[name + "_output"]
            src, dst = Path(temp) / "input.f32", Path(temp) / "output.f32"
            x.T.astype("<f4").tofile(src)
            subprocess.run([str(args.renderer.resolve()), str(src), str(dst), str(spec["depth"]), str(args.block_size)], check=True)
            y = np.fromfile(dst, dtype="<f4").reshape(-1, 2).T
            if y.shape != r.shape or not np.all(np.isfinite(y)):
                raise ValueError(f"invalid native output for {name}")
            n = y.shape[1] // 480 * 480
            yr = np.sqrt(np.mean(y[:, :n].reshape(2, -1, 480).astype(np.float64) ** 2, axis=2))
            rr = np.sqrt(np.mean(r[:, :n].reshape(2, -1, 480).astype(np.float64) ** 2, axis=2))
            valid = rr > 1e-8
            errors = db(yr[valid]) - db(rr[valid])
            result["cases"][name] = dict(purpose=spec["purpose"], native_peak=float(np.max(abs(y))),
                reference_peak=float(np.max(abs(r))), peak_error_db=float(db(np.max(abs(y)) / np.max(abs(r)))),
                envelope_rmse_db=float(rms(errors)), envelope_max_error_db=float(np.max(abs(errors))))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))


def export_tests(args):
    """Export small measured fixtures; tests need neither Python nor the VST."""
    with np.load(args.reference, allow_pickle=False) as ref:
        metadata = json.loads(str(ref["metadata"]))
        lines = ["#pragma once", "// Generated by tools/ott_dynamics.py export-tests.",
                 "// Xfer capture SHA256: " + hashlib.sha256(args.reference.read_bytes()).hexdigest(),
                 "struct OttReferenceWindow { int start, end; float rms; };",
                 "struct OttDynamicReference { const char* name; int hz, depth, frames; float peak; OttReferenceWindow windows[18]; };",
                 "static const OttDynamicReference kOttDynamicReferences[] = {"]
        for name in ("step_40", "step_500", "step_8000", "plucks", "plucks_depth25", "plucks_depth50"):
            y = ref[name + "_output"][0]
            hz = int(name.split("_")[1]) if name.startswith("step_") else 0
            depth = metadata["cases"][name]["depth"]
            if hz:
                windows = [(second * SR + a * 48, second * SR + b * 48)
                           for second in (1, 2, 3)
                           for a, b in ((0, 10), (10, 30), (30, 100), (100, 300), (300, 800), (800, 1000))]
            else:
                windows = [(int(onset * SR) + a * 48, int(onset * SR) + b * 48)
                           for onset in (.25, .75, 1.25)
                           for a, b in ((0, 10), (10, 30), (30, 100), (100, 200), (200, 300), (300, 400))]
            lines.append(f'    {{"{name}", {hz}, {depth}, {len(y)}, {np.max(abs(y)):.9e}f, {{')
            lines.extend(f"        {{{a}, {b}, {rms(y[a:b]):.9e}f}}," for a, b in windows)
            lines.append("    }},")
        lines.extend(["};", ""])
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text("\n".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(required=True)
    cap = sub.add_parser("capture")
    cap.add_argument("plugin", type=Path)
    cap.add_argument("output", type=Path)
    cap.add_argument("--branches", action="store_true")
    cap.set_defaults(func=capture)
    cmp = sub.add_parser("compare")
    cmp.add_argument("reference", type=Path)
    cmp.add_argument("renderer", type=Path)
    cmp.add_argument("output", type=Path)
    cmp.add_argument("--block-size", type=int, choices=(4, 16, 32, 48, 64), default=32)
    cmp.set_defaults(func=compare)
    export = sub.add_parser("export-tests")
    export.add_argument("reference", type=Path)
    export.add_argument("output", type=Path)
    export.set_defaults(func=export_tests)
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
