#!/usr/bin/env python3
"""Derive a local 40-band Modulator/Precise Vocoder measurement preset.

Reads the installed Filterbank preset, preserving Live's native device schema.
The device keeps its displayed name "Filterbank" for measurement manifests.
Envelope times in the .adv XML are milliseconds, unlike the logarithmic raw
values exposed by some Live API parameters.
"""

from __future__ import annotations

import argparse
import gzip
import json
import math
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from tools.live_probe.bridge import local_path
    from tools.live_probe.build_live_set import DEFAULT_RESOURCES, read_xml, value
else:
    from .bridge import local_path
    from .build_live_set import DEFAULT_RESOURCES, read_xml, value


def prepare_vocoder(output: Path, resources: Path = DEFAULT_RESOURCES,
                    source: Path | None = None, *, attack_ms: float = 10,
                    release_ms: float = 30) -> dict:
    source = (source or resources / "Core Library/Devices/Audio Effects/Vocoder/Filterbank.adv").resolve()
    output = output.expanduser().resolve()
    if output == source:
        raise ValueError("Output must be separate from the installed source preset")
    if output.suffix.lower() != ".adv":
        raise ValueError("Output must have the .adv extension")
    if not math.isfinite(attack_ms) or not 1 <= attack_ms <= 1000:
        raise ValueError("attack_ms must be finite and within 1..1000")
    if not math.isfinite(release_ms) or not 10 <= release_ms <= 30000:
        raise ValueError("release_ms must be finite and within 10..30000")
    root = read_xml(source)
    if root.tag != "Ableton" or len(root) != 1 or root[0].tag != "Vocoder":
        raise ValueError("The source preset must contain one native Vocoder")
    vocoder = root[0]
    changes = {
        "UserName": "Filterbank",
        "On/Manual": "true",
        "CarrierSource/Type": "2",
        "Retro/Manual": "false",
        # Native menu [4,8,12,16,20,24,28,32,36,40], confirmed by selecting
        # 40 in Live 12.4.3 and reading the resulting saved set.
        "FilterBank/BandCount": "9",
        "CarrierFlatten/Manual": "true",
        "LowFrequency/Manual": "20",
        "HighFrequency/Manual": "18000",
        "ModulatorAmount/Manual": "1",
        "FilterBandWidth/Manual": "1",
        "FormantShift/Manual": "0",
        "EnvelopeRate/Manual": str(attack_ms),
        "EnvelopeRelease/Manual": str(release_ms),
        "OutputGain/Manual": "0",
        "DryWet/Manual": "1",
        "UvdLevel/Manual": "0",
    }
    for path, setting in changes.items():
        value(vocoder, path, setting)
    for index in range(40):
        value(vocoder, f"FilterBank/BandLevel.{index}", 1)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(gzip.compress(ET.tostring(root, encoding="utf-8", xml_declaration=True), mtime=0))
    report = {
        "preset": str(output), "source": str(source), "device_name": "Filterbank",
        "carrier": "Modulator", "mode": "Precise", "bands": 40, "enhance": True,
        "low_hz": 20, "high_hz": 18000, "attack_ms": attack_ms,
        "release_ms": release_ms, "depth_percent": 100, "bandwidth_percent": 100,
        "formant_semitones": 0, "output_db": 0, "dry_wet_percent": 100,
        "unvoiced_linear": 0,
        "notes": ["The installed Vocoder's high-frequency control reaches 18000 Hz."],
    }
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=local_path, required=True)
    parser.add_argument("--resources", type=local_path, default=DEFAULT_RESOURCES)
    parser.add_argument("--source", type=local_path, help="Alternative installed Filterbank.adv")
    parser.add_argument("--attack-ms", type=float, default=10)
    parser.add_argument("--release-ms", type=float, default=30)
    parser.add_argument("--force", action="store_true", help="Replace an existing generated preset")
    args = parser.parse_args()
    if args.output.exists() and not args.force:
        parser.error(f"Output already exists; choose another --output or use --force: {args.output}")
    try:
        report = prepare_vocoder(args.output, args.resources, args.source,
                                 attack_ms=args.attack_ms, release_ms=args.release_ms)
    except (OSError, ValueError, ET.ParseError) as exc:
        parser.exit(1, f"Cannot prepare Vocoder: {exc}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
