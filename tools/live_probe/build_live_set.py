#!/usr/bin/env python3
"""Prepare a silent, empty audio track using Live's installed default set.

Open the result in Live, add Live Probe Source.amxd, the effect to measure, and
Live Probe Capture.amxd, then save it. Live must create its own Max device state;
constructing Max wrappers directly in .als XML is not reliable.

No Ableton XML is redistributed. The stock set already contains exactly one
audio track and no devices. This script preserves its document structure, IDs,
and empty containers, changing only ordinary mixer/routing/name values.
"""

from __future__ import annotations

import argparse
import gzip
import os
from pathlib import Path
import xml.etree.ElementTree as ET


DEFAULT_RESOURCES = Path(
    "C:/ProgramData/Ableton/Live 12 Suite/Resources"
    if os.name == "nt"
    else "/mnt/c/ProgramData/Ableton/Live 12 Suite/Resources"
)
DEFAULT_TEMPLATE = Path("Core Library/Defaults/Creating Tracks/Audio Track/Default Audio Track.als")


def read_xml(path: Path) -> ET.Element:
    data = path.read_bytes()
    if data.startswith(b"\x1f\x8b"):
        data = gzip.decompress(data)
    return ET.fromstring(data)


def require(element: ET.Element, path: str) -> ET.Element:
    result = element.find(path)
    if result is None:
        raise ValueError(f"Installed Live template lacks {element.tag}/{path}")
    return result


def value(element: ET.Element, path: str, new_value: str | int) -> None:
    require(element, path).set("Value", str(new_value))


def validate(root: ET.Element) -> None:
    tracks = require(root, "LiveSet/Tracks")
    if len(tracks) != 1 or tracks[0].tag != "AudioTrack":
        raise ValueError("The seed must contain exactly one empty audio track")
    if root.findall(".//DeviceChain/Devices/*"):
        raise ValueError("The seed must have no devices; add devices in Live")
    for tag in ("AudioClip", "MidiClip"):
        if next(root.iter(tag), None) is not None:
            raise ValueError("The seed must have no audio or MIDI clips")


def build_live_set(
    workspace: Path,
    output: Path,
    resources: Path = DEFAULT_RESOURCES,
    template: Path | None = None,
) -> Path:
    """Write the empty, silent setup set without launching or controlling Live."""
    workspace = workspace.expanduser().resolve()
    output = output.expanduser().resolve()
    template = (template or resources.expanduser().resolve() / DEFAULT_TEMPLATE).resolve()
    if output.suffix.lower() != ".als":
        raise ValueError("Output must have the .als extension")
    if output == template:
        raise ValueError("Output must be separate from the installed template")
    root = read_xml(template)
    validate(root)
    live = require(root, "LiveSet")
    track = require(live, "Tracks/AudioTrack")
    value(track, "Name/EffectiveName", "Live Probe")
    value(track, "Name/UserName", "Live Probe")
    value(track, "Name/Annotation", "Add Source, the measured effect, then Capture. Main output is silent.")
    value(track, "DeviceChain/MainSequencer/MonitoringEnum", 0)  # Live's In mode
    value(track, "DeviceChain/MainSequencer/Recorder/IsArmed", "false")
    value(track, "DeviceChain/AudioInputRouting/Target", "AudioIn/None")
    value(track, "DeviceChain/AudioInputRouting/UpperDisplayString", "No Input")
    value(track, "DeviceChain/AudioInputRouting/LowerDisplayString", "")
    value(track, "DeviceChain/Mixer/Volume/Manual", 1)
    value(track, "DeviceChain/Mixer/Speaker/Manual", "true")
    main = live.find("MainTrack")
    if main is None:
        main = require(live, "MasterTrack")
    value(main, "DeviceChain/Mixer/Volume/Manual", 0)
    validate(root)
    output.parent.mkdir(parents=True, exist_ok=True)
    xml = ET.tostring(root, encoding="utf-8", xml_declaration=True)
    output.write_bytes(gzip.compress(xml, mtime=0))
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True, help="Prepared Live Probe workspace")
    parser.add_argument("--output", type=Path, help="Destination .als; defaults to workspace/Live Probe Setup.als")
    parser.add_argument("--resources", type=Path, default=DEFAULT_RESOURCES, help="Installed Live Resources directory")
    parser.add_argument("--template", type=Path, help="Alternative stock set with exactly one empty audio track")
    parser.add_argument("--force", action="store_true", help="Replace an existing generated destination set")
    args = parser.parse_args()
    output = args.output or args.workspace / "Live Probe Setup.als"
    if output.exists() and not args.force:
        parser.error(f"Output already exists; choose another --output or use --force: {output}")
    try:
        result = build_live_set(args.workspace, output, args.resources, args.template)
    except (OSError, ValueError, ET.ParseError) as exc:
        parser.exit(1, f"Cannot prepare Live set: {exc}\n")
    print(result)
    print("Open in Live, add Source -> effect -> Capture, then save as your reusable measurement set.")


if __name__ == "__main__":
    main()
