#!/usr/bin/env python3
"""Prepare a Live Probe workspace and optionally load it in Windows Live.

Run without --launch to generate the devices and empty seed set. With --launch,
Live must be closed: the script opens only the generated set, then inserts the
devices using Live's native command-line file opening. It saves the completed
set through Live's native menu command without keyboard input or focus changes,
and verifies the saved device chain. Reopen that set for later measurements.
Automatic launch/save is intended for Windows Live 12 Suite with English menus.
"""

from __future__ import annotations

import argparse
import base64
import json
import math
from pathlib import Path
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.live_probe.bridge import BridgeError, Endpoint, host_path, local_path, session_lock
    from tools.live_probe.build_devices import build as build_devices
    from tools.live_probe.build_live_set import DEFAULT_RESOURCES, build_live_set, read_xml
else:
    from .bridge import BridgeError, Endpoint, host_path, local_path, session_lock
    from .build_devices import build as build_devices
    from .build_live_set import DEFAULT_RESOURCES, build_live_set, read_xml


def ps_literal(text: str) -> str:
    return "'" + text.replace("'", "''") + "'"


def powershell(script: str) -> str:
    encoded = base64.b64encode(script.encode("utf-16-le")).decode("ascii")
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
        check=True, capture_output=True, text=True, encoding="utf-8",
        timeout=30,
    )
    return result.stdout.strip().lstrip("\ufeff")


def live_processes(executable: Path) -> list[dict]:
    name = executable.stem
    script = (
        "$ErrorActionPreference='Stop'; "
        "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new(); "
        f"@(Get-Process -Name {ps_literal(name)} -ErrorAction SilentlyContinue | "
        "Select-Object Id,MainWindowTitle) | ConvertTo-Json -Compress"
    )
    text = powershell(script)
    if not text:
        return []
    data = json.loads(text)
    return data if isinstance(data, list) else [data]


def open_in_live(executable: Path, document: Path) -> None:
    # Windows Live routes subsequent launches to its existing instance. Quote
    # the document for Start-Process's single reconstructed argument string.
    argument = '"' + host_path(document, "windows") + '"'
    powershell(
        "$ErrorActionPreference='Stop'; "
        f"Start-Process -FilePath {ps_literal(host_path(executable, 'windows'))} "
        f"-ArgumentList {ps_literal(argument)}"
    )


def wait_for_set(executable: Path, document: Path, timeout: float) -> None:
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be a positive finite number")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        matches = [item for item in live_processes(executable)
                   if document.stem.casefold() in item.get("MainWindowTitle", "").casefold()]
        if len(matches) == 1:
            # The set title can update just before its empty track is ready.
            time.sleep(1)
            return
        time.sleep(0.5)
    raise RuntimeError(f"Live did not finish opening {document.name}; check its window for a dialog")


def wait_for_bridge(root: Path, role: str, timeout: float) -> dict:
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be a positive finite number")
    endpoint = Endpoint(root, role, timeout=2)
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            response = endpoint.request("ping")
            if response.get("role") != role or response.get("protocol") != 1:
                raise RuntimeError(f"Unexpected {role} bridge response: {response}")
            return response
        except BridgeError as exc:
            # A request published before device initialization receives a
            # restart response. Unanswered requests are retained by Endpoint.
            last_error = exc
            time.sleep(0.25)
    raise RuntimeError(f"The {role} device did not become ready: {last_error}")


def verify_saved_chain(seed: Path, preset: Path | None) -> list[str]:
    root = read_xml(seed)
    tracks = root.find("LiveSet/Tracks")
    if tracks is None or len(tracks) != 1 or tracks[0].tag != "AudioTrack":
        raise RuntimeError("The saved measurement set does not contain exactly one audio track")
    chain = tracks[0].find("DeviceChain/DeviceChain/Devices")
    expected_count = 2 if preset is None else 3
    if chain is None or len(chain) != expected_count:
        raise RuntimeError(f"The saved set does not contain the expected {expected_count} devices")
    for device, expected_name in ((chain[0], "Live Probe Source.amxd"),
                                  (chain[-1], "Live Probe Capture.amxd")):
        if device.tag != "MxDeviceAudioEffect":
            raise RuntimeError(f"Expected a saved Max Audio Effect for {expected_name}")
        reference = device.find("PatchSlot/Value/MxPatchRef/FileRef/Path")
        if reference is None or reference.get("Value", "").replace("\\", "/").split("/")[-1] != expected_name:
            raise RuntimeError(f"Saved bridge file reference differs from {expected_name}")
    if preset is not None and chain[1].tag != read_xml(preset)[0].tag:
        raise RuntimeError("The saved target effect differs from the requested preset")
    return [device.tag for device in chain]


def prepare_project_directory(workspace: Path) -> None:
    # Live's project marker prevents Save from moving an external seed into a
    # newly created "<set name> Project" directory. No icon or binary is needed.
    (workspace / "Ableton Project Info").mkdir(parents=True, exist_ok=True)


def launch_chain(workspace: Path, seed: Path, executable: Path,
                 preset: Path | None, timeout: float) -> dict:
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be a positive finite number")
    if live_processes(executable):
        raise RuntimeError("Live is already running. Save and close it before --launch, or add the prepared devices to your open set manually.")
    print(f"Opening {seed.name}...", flush=True)
    open_in_live(executable, seed)
    wait_for_set(executable, seed, timeout)
    bridge = workspace / "bridge"
    with session_lock(bridge):
        print("Loading Source...", flush=True)
        open_in_live(executable, workspace / "devices/Live Probe Source.amxd")
        source = wait_for_bridge(bridge, "source", timeout)
        if preset is not None:
            print(f"Loading {preset.name}...", flush=True)
            open_in_live(executable, preset)
            time.sleep(2)
        print("Loading Capture...", flush=True)
        open_in_live(executable, workspace / "devices/Live Probe Capture.amxd")
        capture = wait_for_bridge(bridge, "capture", timeout)
        devices = Endpoint(bridge, "source", timeout=10).request("devices")["devices"]
        if preset is None and devices:
            raise RuntimeError(f"Calibration chain contains unexpected devices: {devices}")
        if preset is not None and len(devices) != 1:
            raise RuntimeError(f"Expected one target between Source and Capture, found: {devices}")
        if preset is not None:
            expected_class = read_xml(preset)[0].tag
            if devices[0].get("class_name") != expected_class:
                raise RuntimeError(f"Target class differs from preset: expected {expected_class}, got {devices[0]}")
        from tools.live_probe.native_save import save_live_set
        print("Saving the reusable measurement set...", flush=True)
        saved = save_live_set(seed, timeout=min(timeout, 30))
        saved_chain = verify_saved_chain(seed, preset)
        report = {"source": source, "capture": capture, "devices": devices,
                  "saved_set": saved, "saved_chain": saved_chain}
        (workspace / "setup-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", required=True, type=local_path)
    parser.add_argument("--resources", type=local_path, default=DEFAULT_RESOURCES)
    parser.add_argument("--live-exe", type=local_path, help="Defaults to Resources/../Program/Ableton Live 12 Suite.exe")
    parser.add_argument("--device-preset", type=local_path, help="Optional native audio-effect .adv preset; omit for calibration")
    parser.add_argument("--set-name", default="Live Probe Setup.als", help="New setup set filename")
    parser.add_argument("--launch", action="store_true", help="Start closed Windows Live and insert devices")
    parser.add_argument("--timeout", type=float, default=120, help="Maximum seconds for each startup stage")
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    seed_name = Path(args.set_name)
    if seed_name.name != args.set_name or seed_name.suffix.lower() != ".als":
        parser.error("--set-name must be a plain .als filename")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be a positive finite number")
    seed = workspace / seed_name
    executable = args.live_exe or args.resources.parent / "Program/Ableton Live 12 Suite.exe"
    if seed.exists():
        parser.error(f"{seed} already exists; reopen that saved set, or choose a new --set-name")
    try:
        host_path(workspace, "windows")
        if args.device_preset is not None:
            if args.device_preset.suffix.lower() != ".adv" or not args.device_preset.is_file():
                raise ValueError("--device-preset must point to an existing audio-effect .adv preset")
            preset_root = read_xml(args.device_preset)
            if preset_root.tag != "Ableton" or len(preset_root) != 1:
                raise ValueError("The preset must contain one native audio effect")
        if args.launch:
            if not executable.is_file():
                raise FileNotFoundError(f"Live executable not found: {executable}")
            # Check before rebuilding bridge files belonging to a running set.
            if live_processes(executable):
                raise RuntimeError("Live is already running. Save and close it before --launch.")
        prepare_project_directory(workspace)
        build_devices(workspace)
        build_live_set(workspace, seed, args.resources)
        print(f"Prepared {seed}", flush=True)
        if args.launch:
            report = launch_chain(workspace, seed, executable, args.device_preset, args.timeout)
            print(json.dumps(report, indent=2))
            if not all(report[role].get("dsp_running") for role in ("source", "capture")):
                print("Enable Live's audio engine before recording measurements.")
            print("The chain is ready and saved. Reopen this set for future measurement runs.")
        else:
            print("Open the set in Live, add Source -> optional target -> Capture, and save once.")
            print("For unattended insertion, close Live and rerun with --launch and a new --set-name.")
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, ET.ParseError) as exc:
        parser.exit(1, f"Setup stopped: {exc}\n")


if __name__ == "__main__":
    main()
