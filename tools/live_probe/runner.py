"""Prepare and execute reusable measurement manifests."""

from __future__ import annotations

import hashlib
import itertools
import json
import re
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

from .bridge import BridgeError, Endpoint, atomic_json, host_path, read_json, session_lock
from .signals import finite_number, wav_info, wav_statistics, write_signal


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def fingerprint(value: Any) -> str:
    raw = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def slug(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]+", "-", value).strip("-")[:48] or "signal"


def parameter_values(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object mapping parameter names to values")
    for name, setting in value.items():
        if not isinstance(name, str) or not name:
            raise ValueError(f"{label} names must be nonempty strings")
        if isinstance(setting, (int, float)) and not isinstance(setting, bool):
            finite_number(setting, f"{label}.{name}")
        elif isinstance(setting, str) and setting:
            pass
        elif isinstance(setting, dict) and set(setting) == {"normalized"}:
            finite_number(setting["normalized"], f"{label}.{name}.normalized", minimum=0, maximum=1)
        elif isinstance(setting, dict) and set(setting) == {"display"} and isinstance(setting["display"], str) and setting["display"]:
            pass
        else:
            raise ValueError(f"{label}.{name}: use a raw number, enum label, {{normalized:0..1}}, or {{display:'...'}}")
    return value


def validate_manifest(manifest: Any) -> dict[str, Any]:
    if not isinstance(manifest, dict) or manifest.get("version", 1) != 1:
        raise ValueError("Manifest must be an object with version 1")
    allowed = {"version", "name", "device", "sample_rate", "settle_seconds", "tail_seconds",
               "preroll_seconds", "parameters", "sweep", "cases", "signals", "notes"}
    unknown = set(manifest) - allowed
    if unknown:
        raise ValueError(f"Unknown manifest fields: {', '.join(sorted(unknown))}")
    device = manifest.get("device")
    if device is not None and not ((isinstance(device, str) and device) or
            (isinstance(device, dict) and set(device) in ({"path"}, {"name"}) and
             isinstance(next(iter(device.values())), str) and next(iter(device.values())))):
        raise ValueError("device must be null, an exact device name, {name:'...'}, or {path:'Live API canonical path'}")
    sample_rate = manifest.get("sample_rate", 48000)
    if not isinstance(sample_rate, int) or isinstance(sample_rate, bool):
        raise ValueError("sample_rate must be an integer")
    finite_number(sample_rate, "sample_rate", minimum=8000, maximum=192000)
    for name, default in (("settle_seconds", 0.5), ("tail_seconds", 0.5), ("preroll_seconds", 0.1)):
        finite_number(manifest.get(name, default), name, minimum=0, maximum=120)
    parameter_values(manifest.get("parameters", {}), "parameters")
    sweep = manifest.get("sweep", {})
    if not isinstance(sweep, dict):
        raise ValueError("sweep must be an object mapping parameter names to lists")
    if device is None and (manifest.get("parameters") or sweep):
        raise ValueError("A device selector is required when setting or sweeping parameters")
    count = 1
    for name, values in sweep.items():
        if not isinstance(values, list) or not values:
            raise ValueError(f"sweep.{name} must be a nonempty list")
        for value in values:
            parameter_values({name: value}, "sweep")
        count *= len(values)
    if "cases" in manifest:
        explicit_cases = manifest["cases"]
        if not isinstance(explicit_cases, list) or not explicit_cases:
            raise ValueError("cases must be a nonempty list of parameter-override objects")
        if sweep:
            raise ValueError("cases cannot be combined with a nonempty sweep")
        for index, overrides in enumerate(explicit_cases):
            parameter_values(overrides, f"cases[{index}]")
        if device is None and any(explicit_cases):
            raise ValueError("A device selector is required when setting case parameters")
        count = len({fingerprint({**manifest.get("parameters", {}), **overrides})
                     for overrides in explicit_cases})
    signals = manifest.get("signals")
    if not isinstance(signals, list) or not signals:
        raise ValueError("signals must be a nonempty list")
    names = set()
    for index, spec in enumerate(signals):
        if not isinstance(spec, dict):
            raise ValueError(f"signals[{index}] must be an object")
        name = spec.get("name", f"signal-{index + 1}")
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("Signal names must be unique nonempty strings")
        names.add(name)
    if count * len(signals) > 10000:
        raise ValueError("A manifest may contain at most 10,000 expanded cases")
    return manifest


def prepare_run(manifest_path: Path, output_root: Path) -> tuple[Path, dict[str, Any]]:
    manifest_path = manifest_path.resolve()
    manifest = validate_manifest(read_json(manifest_path))
    run_name = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    run_directory = output_root.resolve() / run_name
    run_directory.mkdir(parents=True, exist_ok=False)
    signals = []
    for index, spec in enumerate(manifest["signals"]):
        name = spec.get("name", f"signal-{index + 1}")
        relative = Path("inputs") / f"{index + 1:03d}-{slug(name)}.wav"
        info = write_signal(spec, run_directory / relative,
                            manifest.get("sample_rate", 48000), manifest_path.parent)
        signals.append({"name": name, "spec": spec, "path": relative.as_posix(),
                        "sha256": file_sha256(run_directory / relative),
                        "audio": info})
    sweep = manifest.get("sweep", {})
    overrides_list = (manifest["cases"] if "cases" in manifest else
                      (dict(zip(sweep, values)) for values in itertools.product(*sweep.values())))
    cases = []
    seen_parameters = set()
    for overrides in overrides_list:
        parameters = {**manifest.get("parameters", {}), **overrides}
        if "cases" in manifest:
            signature = fingerprint(parameters)
            if signature in seen_parameters:
                continue
            seen_parameters.add(signature)
        for signal_index, signal in enumerate(signals):
            case_id = f"{len(cases) + 1:04d}-{slug(signal['name'])}"
            cases.append({"id": case_id, "signal_index": signal_index, "parameters": parameters,
                          "directory": f"cases/{case_id}"})
    plan = {"version": 1, "created_utc": utc_now(), "manifest_path": str(manifest_path),
            "manifest_sha256": fingerprint(manifest), "manifest": manifest,
            "signals": signals, "cases": cases}
    atomic_json(run_directory / "run.json", plan)
    return run_directory, plan


def resume_run(run_directory: Path, manifest_path: Path | None = None) -> dict[str, Any]:
    plan = read_json(run_directory / "run.json")
    validate_manifest(plan["manifest"])
    if fingerprint(plan["manifest"]) != plan["manifest_sha256"]:
        raise ValueError("Saved manifest was modified; prepare a new run")
    if manifest_path is not None and fingerprint(read_json(manifest_path)) != plan["manifest_sha256"]:
        raise ValueError("Resume manifest differs from the saved run")
    for signal in plan["signals"]:
        path = run_directory / signal["path"]
        if file_sha256(path) != signal["sha256"]:
            raise ValueError(f"Prepared input was modified: {path}")
    return plan


def case_complete(run_directory: Path, case: dict[str, Any]) -> bool:
    directory = run_directory / case["directory"]
    metadata_path = directory / "metadata.json"
    if not metadata_path.exists():
        return False
    metadata = read_json(metadata_path)
    if metadata.get("status") != "complete":
        return False
    info = wav_info(directory / "output.wav")
    if not info["frames"] or info != metadata.get("output_audio"):
        raise ValueError(f"Completed output changed or is incomplete: {directory / 'output.wav'}")
    return True


def original_parameters(descriptors: list[dict[str, Any]], names: set[str]) -> dict[str, Any]:
    result = {}
    for name in sorted(names):
        matches = [parameter for parameter in descriptors
                   if name in (parameter.get("name"), parameter.get("original_name"), parameter.get("path"))]
        if len(matches) != 1:
            raise ValueError(f"Parameter {name!r} has {len(matches)} matches; inspect parameters first")
        if matches[0].get("is_enabled") is False:
            raise ValueError(f"Parameter {name!r} is disabled")
        result[name] = matches[0]["value"]
    return result


def wait_for_wav(path: Path, timeout: float = 5.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    previous = None
    error = None
    while time.monotonic() < deadline:
        try:
            info = wav_info(path)
            if info["frames"] and info == previous:
                return info
            previous = info
        except (OSError, ValueError) as exc:
            error = exc
        time.sleep(0.1)
    raise BridgeError(f"Recording is empty or incomplete: {path}. {error or ''}".strip())


def finish_recording(partial: Path, destination: Path, timeout: float = 5.0) -> None:
    """Allow Windows to release a stopped recording before its final rename."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            partial.replace(destination)
            return
        except PermissionError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.1)


def run_batch(bridge_directory: Path, run_directory: Path, plan: dict[str, Any],
              *, timeout: float = 10.0, progress: Callable[[str], None] = print) -> dict[str, Any]:
    """Run the prepared cases and restore every parameter we touched."""
    source = Endpoint(bridge_directory, "source", timeout)
    capture = Endpoint(bridge_directory, "capture", timeout)
    manifest = plan["manifest"]
    device = manifest.get("device")
    pending = [case for case in plan["cases"] if not case_complete(run_directory, case)]
    if not pending:
        previous_summary = run_directory / "summary.json"
        if previous_summary.exists() and read_json(previous_summary).get("cleanup_errors"):
            raise BridgeError(
                f"All recordings are complete, but the previous cleanup failed. "
                f"Inspect {previous_summary} and session.json for parameter restoration details."
            )
        return {"status": "complete", "completed": len(plan["cases"]), "run_directory": str(run_directory)}
    summary: dict[str, Any] = {"status": "running", "started_utc": utc_now(),
                               "run_directory": str(run_directory), "cleanup_errors": []}
    snapshot = {}
    touched = False
    recording = False
    playing = False
    current_case = None
    with session_lock(bridge_directory):
        try:
            source_info = source.request("ping")
            capture_info = capture.request("ping")
            for role, info in (("source", source_info), ("capture", capture_info)):
                if info.get("protocol", 1) != 1:
                    raise BridgeError(f"Unsupported {role} bridge protocol: {info.get('protocol')}")
                if info.get("dsp_running") is False:
                    raise BridgeError(f"{role}: Live's audio engine is disabled; enable an audio device first")
            if capture_info.get("recording"):
                raise BridgeError("Capture is already recording; stop it before starting a batch")
            platform = source_info.get("platform", "")
            host_path(run_directory, platform)
            state = source.request("status")
            if state.get("playing"):
                raise BridgeError("Source is already playing; stop it before starting a batch")
            source.request("stop")
            descriptors = (source.request("parameters", {"device": device}) if device is not None
                           else {"device": {}, "parameters": []})
            names = {name for case in pending for name in case["parameters"]}
            snapshot = original_parameters(descriptors["parameters"], names)
            # Canonical path prevents an accidental device rename/duplicate from
            # redirecting subsequent commands during this run.
            if descriptors.get("device", {}).get("path"):
                device = {"path": descriptors["device"]["path"]}
            atomic_json(run_directory / "session.json", {
                "started_utc": summary["started_utc"], "source": source_info,
                "capture": capture_info, "audio_state": state,
                "target": descriptors.get("device", device),
                "parameters_before": descriptors["parameters"],
                "restore_values": snapshot,
            })
            for index, case in enumerate(pending):
                current_case = case
                signal = plan["signals"][case["signal_index"]]
                directory = run_directory / case["directory"]
                directory.mkdir(parents=True, exist_ok=True)
                partial = directory / "output.partial.wav"
                # Failed attempts remain available until this explicit resume.
                partial.unlink(missing_ok=True)
                metadata = {"status": "running", "started_utc": utc_now(),
                            "case": case, "input": signal, "device": device,
                            "settle_seconds": manifest.get("settle_seconds", 0.5),
                            "tail_seconds": manifest.get("tail_seconds", 0.5),
                            "preroll_seconds": manifest.get("preroll_seconds", 0.1)}
                atomic_json(directory / "metadata.json", metadata)
                progress(f"[{index + 1}/{len(pending)} pending] {case['id']} {json.dumps(case['parameters'], ensure_ascii=False)}")
                loaded = source.request("load", {"path": host_path(run_directory / signal["path"], platform)})
                duration = finite_number(loaded.get("duration_ms"), "loaded duration_ms", minimum=0.001) / 1000
                expected_duration = signal["audio"]["duration_seconds"]
                if abs(duration - expected_duration) > max(0.02, expected_duration * 0.01):
                    raise BridgeError(f"Loaded audio duration {duration} differs from input duration {expected_duration}")
                effective_parameters = {**snapshot, **case["parameters"]}
                if effective_parameters:
                    touched = True  # Even a rejected set might have partially applied.
                    readback = source.request("set", {"device": device, "parameters": effective_parameters})
                elif device is not None:
                    readback = source.request("parameters", {"device": device})
                else:
                    readback = {"device": None, "parameters": []}
                metadata["parameter_readback"] = readback
                atomic_json(directory / "metadata.json", metadata)
                time.sleep(manifest.get("settle_seconds", 0.5))
                recording = True  # Cleanup is needed even if record acknowledgement is lost.
                record = capture.request("record", {"path": host_path(partial, platform), "channels": 2})
                metadata["record_ack"] = record
                time.sleep(manifest.get("preroll_seconds", 0.1))
                playing = True
                started = source.request("play")
                playback_id = started.get("playback_id")
                if not playback_id:
                    raise BridgeError("Source play response is missing playback_id")
                began = time.monotonic()
                deadline = began + duration + timeout
                while True:
                    status = source.request("status")
                    if status.get("playback_id") != playback_id:
                        raise BridgeError("Source playback was replaced by another command")
                    if not status.get("playing"):
                        break
                    if time.monotonic() >= deadline:
                        raise BridgeError("Source did not finish playback; check Live's audio engine")
                    time.sleep(0.1)
                playing = False
                elapsed = time.monotonic() - began
                if elapsed < max(0, duration - 0.25):
                    raise BridgeError(f"Playback ended early ({elapsed:.3f}s for a {duration:.3f}s input)")
                time.sleep(manifest.get("tail_seconds", 0.5))
                stopped = capture.request("stop")
                recording = False
                output_info = wait_for_wav(partial)
                minimum_duration = expected_duration + manifest.get("tail_seconds", 0.5) - 0.25
                if output_info["duration_seconds"] < minimum_duration:
                    raise BridgeError("Recorded audio is shorter than the input plus requested tail")
                output_statistics = wav_statistics(partial)
                final_output = directory / "output.wav"
                finish_recording(partial, final_output)
                metadata.update({"status": "complete", "finished_utc": utc_now(),
                                 "output_audio": output_info, "record_stop_ack": stopped,
                                 "output_statistics": output_statistics,
                                 "observed_playback_seconds": elapsed})
                atomic_json(directory / "metadata.json", metadata)
                current_case = None
            summary["status"] = "complete"
        except BaseException as exc:
            summary["status"] = "interrupted" if isinstance(exc, KeyboardInterrupt) else "failed"
            summary["error"] = str(exc) or type(exc).__name__
            if current_case is not None:
                metadata_path = run_directory / current_case["directory"] / "metadata.json"
                metadata = read_json(metadata_path) if metadata_path.exists() else {"case": current_case}
                metadata.update({"status": summary["status"], "error": summary["error"], "finished_utc": utc_now()})
                atomic_json(metadata_path, metadata)
            raise
        finally:
            for needed, endpoint, action, arguments in (
                (playing, source, "stop", {}),
                (recording, capture, "stop", {}),
                (touched and bool(snapshot), source, "set", {"device": device, "parameters": snapshot}),
            ):
                if needed:
                    try:
                        endpoint.request(action, arguments)
                    except (BridgeError, OSError, ValueError) as exc:
                        message = f"Cleanup {endpoint.role}.{action} failed: {exc}"
                        summary["cleanup_errors"].append(message)
                        progress(message)
            summary["finished_utc"] = utc_now()
            summary["completed"] = sum(case_complete(run_directory, case) for case in plan["cases"])
            if summary["cleanup_errors"] and summary["status"] == "complete":
                summary["status"] = "complete_with_cleanup_errors"
            atomic_json(run_directory / "summary.json", summary)
    if summary["cleanup_errors"]:
        raise BridgeError(f"Measurements finished, but cleanup failed. See {run_directory / 'summary.json'}")
    return summary
