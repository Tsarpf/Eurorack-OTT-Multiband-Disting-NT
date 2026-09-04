"""Command line entry point; run with python -m tools.live_probe."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any

from .bridge import BridgeError, Endpoint, host_path, local_path, read_json, session_lock, try_read_json
from .runner import parameter_values, prepare_run, resume_run, run_batch


def emit(value: Any) -> None:
    print(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False))


def default_bridge() -> str:
    override = os.environ.get("LIVE_PROBE_BRIDGE")
    if override:
        return override
    if os.name == "nt":
        return str(Path.home() / "Documents" / "Ableton" / "LiveProbe" / "bridge")
    return str(Path.home() / "LiveProbe" / "bridge")


def device_selector(args: argparse.Namespace) -> str | dict[str, str]:
    return {"path": args.device_path} if args.device_path else args.device


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Batch audio measurements through a Max for Live bridge.")
    parser.add_argument("--bridge", type=local_path, default=local_path(default_bridge()),
                        help="Shared workspace/bridge directory (or LIVE_PROBE_BRIDGE)")
    parser.add_argument("--timeout", type=float, default=10, help="Seconds allowed for each bridge command")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("doctor", help="Check paths and bridge connectivity")
    commands.add_parser("status", help="Read source playback status and both bridge identities")
    commands.add_parser("devices", help="List target devices in the probe track")
    for action in ("parameters", "set"):
        sub = commands.add_parser(action, help="Inspect or set device parameters")
        selector = sub.add_mutually_exclusive_group(required=True)
        selector.add_argument("--device", help="Exact device name, e.g. Vocoder")
        selector.add_argument("--device-path", help="Canonical path returned by devices")
        if action == "set":
            sub.add_argument("values", help="JSON parameter mapping, or @path/to/values.json")
    run = commands.add_parser("run", help="Prepare and execute a measurement manifest")
    run.add_argument("manifest", type=local_path, nargs="?", help="Manifest JSON path")
    run.add_argument("--output-root", type=local_path, help="Parent of unique run directory (default: workspace/runs)")
    run.add_argument("--resume", type=local_path, help="Resume an existing run directory")
    run.add_argument("--dry-run", action="store_true", help="Generate probes and case plan without contacting Live")
    return parser


def doctor(bridge: Path, timeout: float) -> tuple[dict[str, Any], bool]:
    report: dict[str, Any] = {"bridge_directory": str(bridge), "max_path": host_path(bridge),
                              "exists": bridge.is_dir(), "endpoints": {}}
    if not bridge.is_dir():
        report["error"] = "Bridge directory is missing. Build/install the devices into a shared workspace first."
        return report, False
    lock = bridge / "client.lock"
    if lock.exists():
        report["client_lock"] = try_read_json(lock)
        report["error"] = "A client lock exists. Inspect it before starting another batch."
        return report, False
    success = True
    with session_lock(bridge):
        for role in ("source", "capture"):
            try:
                result = Endpoint(bridge, role, timeout).request("ping")
                report["endpoints"][role] = result
                host_path(bridge, result.get("platform", ""))
            except (BridgeError, OSError, ValueError) as exc:
                success = False
                report["endpoints"][role] = {"error": str(exc)}
    return report, success


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0 or args.timeout > 300:
        parser.error("--timeout must be in (0, 300]")
    bridge = args.bridge.resolve()
    try:
        if args.command == "doctor":
            report, success = doctor(bridge, args.timeout)
            emit(report)
            return 0 if success else 1
        if args.command == "run":
            if args.resume:
                if args.output_root:
                    parser.error("--output-root cannot be combined with --resume")
                run_directory = args.resume.resolve()
                plan = resume_run(run_directory, args.manifest)
            else:
                if args.manifest is None:
                    parser.error("run requires a manifest or --resume")
                run_directory, plan = prepare_run(args.manifest, args.output_root or bridge.parent / "runs")
            print(f"Run directory: {run_directory}", file=sys.stderr)
            if args.dry_run:
                emit({"status": "prepared", "run_directory": str(run_directory),
                      "case_count": len(plan["cases"]), "signal_count": len(plan["signals"]),
                      "plan": str(run_directory / "run.json")})
            else:
                emit(run_batch(bridge, run_directory, plan, timeout=args.timeout,
                               progress=lambda message: print(message, file=sys.stderr, flush=True)))
            return 0
        source = Endpoint(bridge, "source", args.timeout)
        with session_lock(bridge):
            if args.command == "status":
                emit({"source": source.request("ping"),
                      "capture": Endpoint(bridge, "capture", args.timeout).request("ping"),
                      "playback": source.request("status")})
            elif args.command == "devices":
                emit(source.request("devices"))
            elif args.command == "parameters":
                emit(source.request("parameters", {"device": device_selector(args)}))
            elif args.command == "set":
                values = read_json(local_path(args.values[1:])) if args.values.startswith("@") else json.loads(args.values)
                parameter_values(values, "values")
                emit(source.request("set", {"device": device_selector(args), "parameters": values}))
        return 0
    except KeyboardInterrupt:
        print("Interrupted; see the run's summary.json for cleanup results.", file=sys.stderr)
        return 130
    except (BridgeError, OSError, ValueError, KeyError) as exc:
        print(f"live-probe: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
