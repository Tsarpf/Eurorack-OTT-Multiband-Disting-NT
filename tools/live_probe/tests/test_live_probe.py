"""Transport and full-batch tests with a small file-protocol endpoint emulator."""

from __future__ import annotations

import json
import errno
import math
import os
import shutil
import struct
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import wave
from pathlib import Path

from tools.live_probe.bridge import (BridgeError, BridgeTimeout, Endpoint, atomic_json,
                                    host_path, local_path, read_json, session_lock,
                                    try_read_json)
from tools.live_probe.runner import (case_complete, prepare_run, resume_run, run_batch,
                                    validate_manifest)
from tools.live_probe.signals import wav_info, wav_statistics, write_signal


class FakeLive:
    """Exercise the actual JSON/filesystem client, not a mocked Endpoint."""

    def __init__(self, root: Path):
        self.root = root
        self.quit = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.last = {}
        self.commands = []
        self.values = {"Depth": 0.75, "Width": 1.0}
        self.loaded = None
        self.recording = None
        self.playback_id = None
        self.end = 0
        self.fail_play = False
        self.fail_set = False
        self.error = None

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.quit.set()
        self.thread.join(2)
        if self.error:
            raise self.error

    def descriptors(self, names=None):
        return [{"name": name, "value": value, "min": 0, "max": 4,
                 "display_value": f"{value * 100:g} %", "is_enabled": True}
                for name, value in self.values.items() if names is None or name in names]

    def dispatch(self, role, command):
        action, args = command["action"], command["args"]
        if action == "ping":
            return {"protocol": 1, "role": role, "platform": "test", "dsp_running": True,
                    "sample_rate": 48000, "recording": self.recording is not None}
        if action == "status":
            return {"playing": time.monotonic() < self.end, "playback_id": self.playback_id,
                    "sample_rate": 48000}
        if role == "source":
            if action == "parameters":
                return {"device": {"name": "Vocoder", "path": "live_set tracks 0 devices 1"},
                        "parameters": self.descriptors()}
            if action == "set":
                if self.fail_set:
                    raise ValueError("Rejected parameter")
                previous = self.descriptors(args["parameters"])
                for name, value in args["parameters"].items():
                    if isinstance(value, dict):
                        value = value["normalized"] * 4 if "normalized" in value else float(value["display"].split()[0]) / 100
                    self.values[name] = value
                return {"parameters": self.descriptors(args["parameters"]), "previous": previous}
            if action == "load":
                self.loaded = Path(args["path"])
                return {"duration_ms": wav_info(self.loaded)["duration_seconds"] * 1000}
            if action == "play":
                if self.fail_play:
                    raise ValueError("Simulated audio failure")
                self.playback_id = command["id"]
                self.end = time.monotonic() + wav_info(self.loaded)["duration_seconds"]
                return {"playing": True, "playback_id": self.playback_id}
            if action == "stop":
                self.end = 0
                return {"playing": False}
            if action == "devices":
                return {"devices": [{"name": "Vocoder", "path": "live_set tracks 0 devices 1"}]}
        if role == "capture":
            if action == "record":
                self.recording = Path(args["path"])
                return {"path": str(self.recording), "recording": True}
            if action == "stop":
                result = {"path": str(self.recording)}
                if self.recording is not None and self.loaded:
                    shutil.copyfile(self.loaded, self.recording)
                self.recording = None
                return result
        raise ValueError(f"Unexpected command: {role}.{action}")

    def serve(self):
        try:
            while not self.quit.wait(0.003):
                for role in ("source", "capture"):
                    command = try_read_json(self.root / role / "command.json")
                    if not command or self.last.get(role) == command["id"]:
                        continue
                    self.last[role] = command["id"]
                    self.commands.append((role, command))
                    try:
                        result = self.dispatch(role, command)
                        response = {"id": command["id"], "ok": True, "result": result}
                    except ValueError as exc:
                        response = {"id": command["id"], "ok": False, "error": str(exc)}
                    atomic_json(self.root / role / "response.json", response)
        except BaseException as exc:
            self.error = exc


class TransportTests(unittest.TestCase):
    def test_matching_ids_ignore_stale_responses_and_surface_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            atomic_json(root / "source" / "response.json", {"id": "stale", "ok": True, "result": {"stale": True}})
            with FakeLive(root) as server, session_lock(root):
                endpoint = Endpoint(root, "source", timeout=1)
                self.assertEqual(endpoint.request("ping")["role"], "source")
                server.fail_set = True
                with self.assertRaisesRegex(BridgeError, "Rejected parameter"):
                    endpoint.request("set", {"parameters": {"Depth": 1}})

    def test_timeout_preserves_pending_command_instead_of_overwriting(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            endpoint = Endpoint(root, "source", timeout=0.05, poll_interval=0.005)
            with self.assertRaises(BridgeTimeout):
                endpoint.request("ping")
            pending = read_json(root / "source" / "command.json")
            with self.assertRaisesRegex(BridgeError, "unanswered command"):
                endpoint.request("stop")
            self.assertEqual(read_json(root / "source" / "command.json"), pending)
            # The original command can finish later, unblocking further work.
            with FakeLive(root):
                deadline = time.monotonic() + 1
                while try_read_json(root / "source" / "response.json") is None and time.monotonic() < deadline:
                    time.sleep(0.005)
                self.assertEqual(endpoint.request("ping", timeout=1)["role"], "source")

    def test_transient_drvfs_enodata_retries_but_other_io_errors_propagate(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            interrupted_reads = []

            def drvfs_read(path):
                if path.name == "response.json" and path.exists() and not interrupted_reads:
                    interrupted_reads.append(path)
                    raise OSError(getattr(errno, "ENODATA", 61), "No data available")
                return read_json(path)

            with FakeLive(root), patch("tools.live_probe.bridge.read_json", side_effect=drvfs_read):
                result = Endpoint(root, "source", timeout=1).request("ping")
                self.assertEqual(result["role"], "source")
                self.assertEqual(len(interrupted_reads), 1)
            with patch("tools.live_probe.bridge.read_json", side_effect=OSError(errno.EIO, "I/O error")):
                with self.assertRaises(OSError) as raised:
                    try_read_json(root / "source" / "response.json")
                self.assertEqual(raised.exception.errno, errno.EIO)

    def test_client_lock_is_exclusive_and_released_on_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(ValueError, "intentional"):
                with session_lock(root):
                    with self.assertRaises(BridgeError):
                        with session_lock(root):
                            self.fail("Second client acquired the lock")
                    raise ValueError("intentional")
            self.assertFalse((root / "client.lock").exists())

    def test_windows_and_wsl_path_mapping(self):
        if os.name == "nt":
            self.assertEqual(str(local_path(r"C:\Users\Some User\probe.wav")), r"C:\Users\Some User\probe.wav")
            self.assertEqual(host_path(Path(r"C:\Users\Some User\probe.wav"), "windows"), "C:/Users/Some User/probe.wav")
            return
        self.assertEqual(str(local_path(r"C:\Users\Some User\probe.wav")), "/mnt/c/Users/Some User/probe.wav")
        self.assertEqual(host_path(Path("/mnt/c/Users/Some User/probe.wav"), "windows"), "C:/Users/Some User/probe.wav")
        with self.assertRaisesRegex(ValueError, "Windows Live cannot open"):
            host_path(Path("/tmp/not-windows.wav"), "windows")


class SignalTests(unittest.TestCase):
    def test_impulse_level_frame_and_stereo_samples(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "impulse.wav"
            info = write_signal({"kind": "impulse", "duration": 0.1, "offset_seconds": 0.01,
                                 "level_dbfs": -6, "right_gain_db": -6}, path, 48000, path.parent)
            self.assertEqual(info["frames"], 4800)
            self.assertEqual(info["channels"], 2)
            with wave.open(str(path), "rb") as audio:
                data = audio.readframes(audio.getnframes())
            left = [int.from_bytes(data[i:i + 3], "little", signed=True) for i in range(0, len(data), 6)]
            right = [int.from_bytes(data[i:i + 3], "little", signed=True) for i in range(3, len(data), 6)]
            self.assertEqual(sum(value != 0 for value in left), 1)
            self.assertAlmostEqual(left[480] / 8388607, 10 ** (-6 / 20), places=6)
            self.assertAlmostEqual(right[480] / left[480], 10 ** (-6 / 20), places=6)
            statistics = wav_statistics(path)
            self.assertAlmostEqual(statistics["channels"][0]["peak_dbfs"], -6, places=5)
            self.assertAlmostEqual(statistics["channels"][0]["rms_dbfs"], -6 - 10 * math.log10(4800), places=5)

    def test_noise_is_deterministic_and_invalid_signal_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ("one", "two"):
                write_signal({"kind": "noise", "duration": 0.02, "seed": 83}, root / f"{name}.wav", 48000, root)
            self.assertEqual((root / "one.wav").read_bytes(), (root / "two.wav").read_bytes())
            with self.assertRaisesRegex(ValueError, "Nyquist"):
                write_signal({"kind": "sine", "frequency": 24000}, root / "bad.wav", 48000, root)
            with self.assertRaisesRegex(ValueError, "Unknown sine signal fields"):
                write_signal({"kind": "sine", "frequncy": 800}, root / "bad.wav", 48000, root)

    def test_stepped_sine_duration_and_actual_level_difference(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "steps.wav"
            write_signal({"kind": "stepped_sine", "frequency": 1000,
                          "levels_dbfs": [-24, -6], "segment_seconds": 0.01}, path, 48000, root)
            with wave.open(str(path), "rb") as audio:
                self.assertEqual(audio.getnframes(), 960)
                data = audio.readframes(960)
            samples = [int.from_bytes(data[i:i + 3], "little", signed=True) for i in range(0, len(data), 6)]
            ratio = math.sqrt(sum(value ** 2 for value in samples[480:]) / sum(value ** 2 for value in samples[:480]))
            self.assertAlmostEqual(20 * math.log10(ratio), 18, places=4)

    def test_float_wav_from_max_and_incomplete_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "float.wav"
            fmt = struct.pack("<HHIIHH", 3, 2, 48000, 384000, 8, 32)
            data = struct.pack("<ffff", 0.1, 0.1, 0, 0)
            body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
            path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
            self.assertEqual(wav_info(path)["encoding"], "float")
            self.assertEqual(wav_info(path)["frames"], 2)
            self.assertAlmostEqual(wav_statistics(path)["channels"][0]["rms"], 0.1 / math.sqrt(2), places=7)
            path.write_bytes(path.read_bytes()[:-3])
            with self.assertRaisesRegex(ValueError, "Incomplete WAV"):
                wav_info(path)

    def test_nonfinite_output_rejected_and_silence_allowed(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "float.wav"
            fmt = struct.pack("<HHIIHH", 3, 1, 48000, 192000, 4, 32)
            data = struct.pack("<ff", float("nan"), 0)
            body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
            path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
            with self.assertRaisesRegex(ValueError, "Non-finite samples"):
                wav_statistics(path)
            write_signal({"kind": "silence", "duration": 0.01}, path, 48000, path.parent)
            self.assertTrue(wav_statistics(path)["silent"])


class BatchTests(unittest.TestCase):
    def make_plan(self, root):
        manifest = {"version": 1, "device": "Vocoder", "sample_rate": 48000,
                    "settle_seconds": 0, "tail_seconds": 0, "preroll_seconds": 0,
                    "parameters": {"Width": {"normalized": 0.5}},
                    "sweep": {"Depth": [{"display": "100 %"}, {"display": "400 %"}]},
                    "signals": [{"name": "test-tone", "kind": "sine", "frequency": 440, "duration": 0.03}]}
        path = root / "manifest.json"
        atomic_json(path, manifest)
        return prepare_run(path, root / "runs")

    def test_full_batch_readback_restoration_and_resume_skip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_directory, plan = self.make_plan(root)
            self.assertEqual(len(plan["cases"]), 2)
            with FakeLive(root / "bridge") as server:
                result = run_batch(root / "bridge", run_directory, plan, timeout=1, progress=lambda _: None)
                self.assertEqual(result["status"], "complete")
                self.assertEqual(server.values, {"Depth": 0.75, "Width": 1.0})
                for case in plan["cases"]:
                    self.assertTrue(case_complete(run_directory, case))
                    metadata = read_json(run_directory / case["directory"] / "metadata.json")
                    self.assertIn("parameter_readback", metadata)
                count = len(server.commands)
                resumed = resume_run(run_directory)
                run_batch(root / "bridge", run_directory, resumed, timeout=1, progress=lambda _: None)
                self.assertEqual(len(server.commands), count, "Completed run should not touch Live on resume")
                summary = read_json(run_directory / "summary.json")
                summary["cleanup_errors"] = ["Parameter restoration failed"]
                atomic_json(run_directory / "summary.json", summary)
                with self.assertRaisesRegex(BridgeError, "previous cleanup failed"):
                    run_batch(root / "bridge", run_directory, resumed, timeout=1, progress=lambda _: None)

    def test_explicit_cases_merge_deduplicate_and_reset_omitted_parameters(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "explicit.json"
            manifest = {"device": "Vocoder", "settle_seconds": 0, "tail_seconds": 0,
                        "preroll_seconds": 0, "parameters": {"Width": 1},
                        "cases": [{}, {"Depth": 2}, {"Width": 3}, {"Width": 1}],
                        "signals": [{"kind": "silence", "duration": 0.01}]}
            atomic_json(path, manifest)
            run_directory, plan = prepare_run(path, root / "runs")
            self.assertEqual([case["parameters"] for case in plan["cases"]],
                             [{"Width": 1}, {"Width": 1, "Depth": 2}, {"Width": 3}])
            with FakeLive(root / "bridge") as server:
                run_batch(root / "bridge", run_directory, plan, timeout=1, progress=lambda _: None)
                settings = [command["args"]["parameters"] for _, command in server.commands
                            if command["action"] == "set"]
                self.assertEqual(settings[2], {"Depth": 0.75, "Width": 3},
                                 "Omitted Depth must return to the snapshot for the Width case")
                self.assertEqual(server.values, {"Depth": 0.75, "Width": 1})
            with self.assertRaisesRegex(ValueError, "nonempty sweep"):
                validate_manifest({**manifest, "sweep": {"Depth": [0, 1]}})

    def test_calibration_without_target_device(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "calibration.json"
            atomic_json(path, {"device": None, "settle_seconds": 0, "tail_seconds": 0,
                               "preroll_seconds": 0, "signals": [{"kind": "sine", "duration": 0.03}]})
            run_directory, plan = prepare_run(path, root / "runs")
            with FakeLive(root / "bridge") as server:
                result = run_batch(root / "bridge", run_directory, plan, timeout=1, progress=lambda _: None)
                self.assertEqual(result["status"], "complete")
                self.assertFalse(any(command["action"] in ("parameters", "set") for _, command in server.commands))

    def test_failure_stops_recording_restores_settings_and_can_resume(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_directory, plan = self.make_plan(root)
            with FakeLive(root / "bridge") as server:
                server.fail_play = True
                with self.assertRaisesRegex(BridgeError, "Simulated audio failure"):
                    run_batch(root / "bridge", run_directory, plan, timeout=1, progress=lambda _: None)
                self.assertIsNone(server.recording)
                self.assertEqual(server.values, {"Depth": 0.75, "Width": 1.0})
                self.assertFalse((root / "bridge" / "client.lock").exists())
                summary = read_json(run_directory / "summary.json")
                self.assertEqual(summary["status"], "failed")
                self.assertEqual(summary["cleanup_errors"], [])
                server.fail_play = False
                result = run_batch(root / "bridge", run_directory, resume_run(run_directory), timeout=1, progress=lambda _: None)
                self.assertEqual(result["completed"], 2)

    def test_resume_detects_modified_input_and_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run_directory, plan = self.make_plan(root)
            alternate = root / "alternate.json"
            atomic_json(alternate, {**plan["manifest"], "device": "Something Else"})
            with self.assertRaisesRegex(ValueError, "differs"):
                resume_run(run_directory, alternate)
            audio = run_directory / plan["signals"][0]["path"]
            audio.write_bytes(b"changed")
            with self.assertRaisesRegex(ValueError, "modified"):
                resume_run(run_directory)

    def test_invalid_manifests_fail_before_bridge_commands(self):
        with self.assertRaisesRegex(ValueError, "Unknown manifest"):
            validate_manifest({"device": "Vocoder", "signals": [{}], "sweeeep": {}})
        with self.assertRaisesRegex(ValueError, "must be finite"):
            validate_manifest({"device": "Vocoder", "signals": [{}], "parameters": {"Depth": float("nan")}})


if __name__ == "__main__":
    unittest.main()
