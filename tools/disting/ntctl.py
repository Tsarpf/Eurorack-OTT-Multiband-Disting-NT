#!/usr/bin/env python3

import argparse
import sys
import time
from pathlib import Path, PurePath

import mido

EXPERT_SLEEPERS_HEADER = [0xF0, 0x00, 0x21, 0x27, 0x6D]

K_OP_DOWNLOAD = 2
K_OP_UPLOAD = 4
K_OP_LISTING = 1
K_OP_DELETE = 3
K_OP_RENAME = 5
K_OP_RESCAN = 8
K_OP_REMOUNT = 6
K_OP_NEW_FOLDER = 7

VOCODER_GUID = b"VOC2"
OTT_GUID     = b"OTT1"
# SysEx parameter lists include the common bypass parameter at index 0.
VOCODER_PARAM_BAND_COUNT = 7

# OTT parameter indices (SysEx index = plugin index + 1, bypass is at 0)
OTT_PARAM_NAMES = [
    "In", "Stereo", "Out", "Out_mode",
    "Hi/DownThr", "Hi/UpThr", "Hi/DownRat", "Hi/UpRat",
    "Hi/PreGain", "Hi/PostGain", "Hi/Attack", "Hi/Release",
    "Mid/DownThr", "Mid/UpThr", "Mid/DownRat", "Mid/UpRat",
    "Mid/PreGain", "Mid/PostGain", "Mid/Attack", "Mid/Release",
    "Lo/DownThr", "Lo/UpThr", "Lo/DownRat", "Lo/UpRat",
    "Lo/PreGain", "Lo/PostGain", "Lo/Attack", "Lo/Release",
    "Xover/LoMid", "Xover/MidHi", "Global/Out", "Global/Wet",
]
# Default raw values matching ott_parameters.h (+17 dB Global/Out makeup)
OTT_PARAM_DEFAULTS = [
    1, 0, 13, 1,                 # routing (mono, replace)
    -100, -300, 400, 200, 0, 0, 135, 1320,   # Hi band
    -100, -300, 400, 200, 0, 0, 224, 2820,   # Mid band
    -100, -300, 400, 200, 0, 0, 478, 2820,   # Lo band
    160, 2500, 170, 100,         # xover + global (+17 dB makeup)
]
VOCODER_PARAM_BAND_WIDTH = 8
VOCODER_PARAM_FORMANT = 10


def pack_u16(value: int) -> list[int]:
    value &= 0xFFFF
    return [(value >> 14) & 0x03, (value >> 7) & 0x7F, value & 0x7F]


def unpack_u16(data: list[int]) -> int:
    if len(data) == 1:
        return data[0]
    if len(data) == 2:
        return (data[0] << 7) | data[1]
    return (data[0] << 14) | (data[1] << 7) | data[2]


def pack_u35(value: int) -> list[int]:
    return [
        0,
        0,
        0,
        0,
        0,
        (value >> 28) & 0x0F,
        (value >> 21) & 0x7F,
        (value >> 14) & 0x7F,
        (value >> 7) & 0x7F,
        value & 0x7F,
    ]


def decode_nibble_bytes(data: list[int]) -> bytes:
    out = bytearray()
    for i in range(0, len(data), 2):
        out.append(((data[i] & 0x0F) << 4) | (data[i + 1] & 0x0F))
    return bytes(out)


def add_checksum(arr: list[int]) -> None:
    total = 0
    for byte in arr[7:]:
        total += byte
    arr.append((-total) & 0x7F)


def make_message(sys_ex_id: int, opcode: int, payload: list[int] | None = None,
                 checksum: bool = False) -> mido.Message:
    arr = EXPERT_SLEEPERS_HEADER + [sys_ex_id, opcode]
    if payload:
        arr.extend(payload)
    if checksum:
        add_checksum(arr)
    arr.append(0xF7)
    return mido.Message.from_bytes(arr)


class DistingClient:
    def __init__(self, sys_ex_id: int, port_hint: str = "disting NT", timeout: float = 2.0):
        self.sys_ex_id = sys_ex_id
        self.timeout = timeout
        outputs = mido.get_output_names()
        inputs = mido.get_input_names()
        try:
            out_name = next(name for name in outputs if port_hint in name)
            in_name = next(name for name in inputs if port_hint in name)
        except StopIteration as exc:
            raise RuntimeError(f"Could not find MIDI ports matching {port_hint!r}") from exc
        self.out_port = mido.open_output(out_name)
        self.in_port = mido.open_input(in_name)

    def close(self) -> None:
        self.in_port.close()
        self.out_port.close()

    def drain(self, duration: float = 0.1) -> None:
        deadline = time.time() + duration
        while time.time() < deadline:
            msg = self.in_port.poll()
            if msg is None:
                time.sleep(0.01)

    def send(self, opcode: int, payload: list[int] | None = None, checksum: bool = False) -> None:
        self.out_port.send(make_message(self.sys_ex_id, opcode, payload, checksum))

    def recv(self, expected_opcode: int | None = None, timeout: float | None = None) -> list[int]:
        deadline = time.time() + (self.timeout if timeout is None else timeout)
        while time.time() < deadline:
            msg = self.in_port.poll()
            if msg is None:
                time.sleep(0.01)
                continue
            data = msg.bytes()
            if data[:6] != EXPERT_SLEEPERS_HEADER + [self.sys_ex_id]:
                continue
            if expected_opcode is not None and data[6] != expected_opcode:
                continue
            return data
        raise TimeoutError(f"Timed out waiting for SysEx opcode {expected_opcode!r}")

    def request(self, request_opcode: int, expected_opcode: int | None = None,
                payload: list[int] | None = None, checksum: bool = False,
                timeout: float | None = None) -> list[int]:
        self.drain()
        self.send(request_opcode, payload, checksum)
        return self.recv(request_opcode if expected_opcode is None else expected_opcode, timeout)

    def version(self) -> str:
        data = self.request(0x22, expected_opcode=0x32)
        text = bytes(data[7:-1]).split(b"\x00")
        return " | ".join(chunk.decode("ascii", errors="replace") for chunk in text if chunk)

    def get_paths(self) -> list[str]:
        data = self.request(0x56, expected_opcode=0x56)
        raw = bytes(data[7:-1])
        return [part.decode("ascii", errors="replace") for part in raw.split(b"\x00") if part]

    def cpu_usage(self) -> tuple[int, int, list[int]]:
        data = self.request(0x62, expected_opcode=0x62)
        payload = data[7:-1]
        return payload[0], payload[1], payload[2:]

    def slot_count(self) -> int:
        data = self.request(0x60, expected_opcode=0x60)
        return unpack_u16(data[7:-1])

    def algorithm_guid(self, slot: int) -> tuple[bytes, str]:
        data = self.request(0x40, expected_opcode=0x40, payload=[slot])
        guid = bytes(data[8:12])
        name = bytes(data[12:-1]).split(b"\x00", 1)[0].decode("ascii", errors="replace")
        return guid, name

    def preset_name(self) -> str:
        data = self.request(0x41, expected_opcode=0x41)
        return bytes(data[7:-1]).split(b"\x00", 1)[0].decode("ascii", errors="replace")

    def all_parameter_values(self, slot: int) -> list[int]:
        data = self.request(0x44, expected_opcode=0x44, payload=[slot])
        payload = data[7:-1]
        if not payload:
            return []
        values = []
        for i in range(1, len(payload), 3):
            group = payload[i:i + 3]
            if len(group) < 3:
                break
            values.append(unpack_u16(group))
        return values

    def parameter_value(self, slot: int, parameter: int) -> int:
        values = self.all_parameter_values(slot)
        return values[parameter]

    def set_parameter_value(self, slot: int, parameter: int, value: int) -> None:
        payload = [slot] + pack_u16(parameter) + pack_u16(value)
        self.send(0x46, payload)

    def new_preset(self) -> None:
        self.send(0x35)

    def set_preset_name(self, name: str) -> None:
        self.send(0x47, [ord(ch) for ch in name] + [0])

    def save_preset(self, overwrite: int = 2) -> None:
        self.send(0x36, [overwrite])
        time.sleep(0.5)

    def load_preset(self, path: str) -> None:
        self.send(0x34, [0] + [ord(ch) for ch in path] + [0])

    def _fs_request(self, op: int, payload: list[int]) -> list[int]:
        self.drain()
        self.send(0x7A, [op] + payload, checksum=True)
        return self.recv(expected_opcode=0x7A)

    def list_dir(self, path: str) -> list[dict]:
        data = self._fs_request(K_OP_LISTING, [ord(ch) for ch in path])
        payload = data[9:-2]
        items = []
        i = 0
        while i < len(payload):
            remaining = len(payload) - i
            if remaining < 18:
                break
            attrib = payload[i]
            i += 1
            date = unpack_u16(payload[i:i + 3])
            i += 3
            tm = unpack_u16(payload[i:i + 3])
            i += 3
            size = 0
            for j in range(10):
                size += payload[i] << ((9 - j) * 7)
                i += 1
            name_bytes = bytearray()
            while i < len(payload):
                byte = payload[i]
                i += 1
                if byte == 0:
                    break
                name_bytes.append(byte)
            name = name_bytes.decode("ascii", errors="replace")
            if not name:
                break
            is_dir = bool(attrib & 0x10)
            items.append({
                "name": name + ("/" if is_dir else ""),
                "is_dir": is_dir,
                "size": size,
                "date": date,
                "time": tm,
            })
        items.sort(key=lambda item: item["name"])
        return items

    def upload_file(self, local_path: Path, nt_path: str) -> None:
        data = local_path.read_bytes()
        ack_prefix = EXPERT_SLEEPERS_HEADER + [self.sys_ex_id, 0x7A, 0, K_OP_UPLOAD]
        pos = 0
        while pos < len(data):
            count = min(512, len(data) - pos)
            payload = [K_OP_UPLOAD]
            payload.extend(ord(ch) for ch in nt_path)
            payload.append(0)
            payload.append(1 if pos == 0 else 0)
            payload.extend(pack_u35(pos))
            payload.extend(pack_u35(count))
            for byte in data[pos:pos + count]:
                payload.append((byte >> 4) & 0x0F)
                payload.append(byte & 0x0F)
            self.drain(0.02)
            self.send(0x7A, payload, checksum=True)
            resp = self.recv(expected_opcode=0x7A, timeout=5.0)
            if resp[:9] != ack_prefix:
                raise RuntimeError(f"Unexpected upload response: {resp}")
            pos += count

    def delete_file(self, nt_path: str) -> None:
        self._fs_request(K_OP_DELETE, [ord(ch) for ch in nt_path])

    def rescan_plugins(self) -> None:
        self._fs_request(K_OP_RESCAN, [])


def cmd_ports(_: argparse.Namespace) -> int:
    print("Outputs:")
    for name in mido.get_output_names():
        print(name)
    print("Inputs:")
    for name in mido.get_input_names():
        print(name)
    return 0


def with_client(args: argparse.Namespace, fn):
    client = DistingClient(args.sys_ex_id, args.port_hint, args.timeout)
    try:
        return fn(client)
    finally:
        client.close()


def cmd_version(args: argparse.Namespace) -> int:
    return with_client(args, lambda client: print(client.version()) or 0)


def cmd_paths(args: argparse.Namespace) -> int:
    def run(client: DistingClient) -> int:
        for path in client.get_paths():
            print(path)
        return 0
    return with_client(args, run)


def cmd_list(args: argparse.Namespace) -> int:
    def run(client: DistingClient) -> int:
        for item in client.list_dir(args.path):
            kind = "dir " if item["is_dir"] else "file"
            print(f"{kind:4} {item['size']:8d} {item['name']}")
        return 0
    return with_client(args, run)


def cmd_cpu(args: argparse.Namespace) -> int:
    def run(client: DistingClient) -> int:
        for idx in range(args.samples):
            alg, whole, slots = client.cpu_usage()
            slot_text = " ".join(str(v) for v in slots)
            print(f"{idx:02d} alg={alg:3d}% module={whole:3d}% slots=[{slot_text}]")
            if idx + 1 < args.samples:
                time.sleep(args.interval)
        return 0
    return with_client(args, run)


def cmd_delete(args: argparse.Namespace) -> int:
    def run(client: DistingClient) -> int:
        client.delete_file(args.nt_path)
        client.rescan_plugins()
        print(f"Deleted {args.nt_path}")
        return 0
    return with_client(args, run)


def cmd_push_plugin(args: argparse.Namespace) -> int:
    local_path = Path(args.local_plugin).resolve()
    nt_plugin_path = "/programs/plug-ins/" + PurePath(local_path).name

    def run(client: DistingClient) -> int:
        paths = client.get_paths()
        current_preset = paths[0] if paths else ""
        if args.save_as:
            client.set_preset_name(args.save_as)
            client.save_preset(2)
            current_preset = client.get_paths()[0]
        client.new_preset()
        client.upload_file(local_path, nt_plugin_path)
        client.rescan_plugins()
        if current_preset:
            client.load_preset(current_preset)
        print(f"Uploaded {local_path} -> {nt_plugin_path}")
        if current_preset:
            print(f"Reloaded preset {current_preset}")
        return 0
    return with_client(args, run)


def find_vocoder_slot(client: DistingClient) -> int:
    slot_count = client.slot_count()
    for slot in range(slot_count):
        guid, name = client.algorithm_guid(slot)
        if guid == VOCODER_GUID:
            return slot
    raise RuntimeError("Could not find a VOC2 vocoder slot in the current preset")


def find_ott_slot(client: DistingClient) -> int:
    slot_count = client.slot_count()
    for slot in range(slot_count):
        guid, name = client.algorithm_guid(slot)
        if guid == OTT_GUID:
            return slot
    raise RuntimeError("Could not find an OTT1 slot in the current preset")


def to_signed16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v >= 0x8000 else v


def cmd_dump_ott(args: argparse.Namespace) -> int:
    def run(client: DistingClient) -> int:
        slot = find_ott_slot(client)
        vals = client.all_parameter_values(slot)  # vals[0]=bypass, vals[1]=plugin param 0
        print(f"OTT slot={slot}  bypass={vals[0]}")
        for i, name in enumerate(OTT_PARAM_NAMES):
            raw = to_signed16(vals[i + 1]) if i + 1 < len(vals) else "?"
            print(f"  [{i:2d}] {name:<20s}  raw={raw}")
        return 0
    return with_client(args, run)


def cmd_init_ott(args: argparse.Namespace) -> int:
    def run(client: DistingClient) -> int:
        slot = find_ott_slot(client)
        _, name = client.algorithm_guid(slot)
        print(f"Resetting OTT slot={slot} ({name}) to defaults")
        for i, (param_name, default) in enumerate(zip(OTT_PARAM_NAMES, OTT_PARAM_DEFAULTS)):
            sysex_idx = i + 1  # +1 because bypass is at index 0
            client.set_parameter_value(slot, sysex_idx, default)
        print("Done — save the preset manually if you want to keep these values")
        return 0
    return with_client(args, run)


def cmd_reset_bands_ott(args: argparse.Namespace) -> int:
    """Reset only band params + xover; leave routing and Global/Out/Wet alone."""
    # Indices of params to reset (0-based plugin param index, not SysEx index)
    # kHiDownThr..kLoRelease = indices 4..29, kXoverLoMid..kXoverMidHi = 30..31
    BAND_INDICES = list(range(4, 32))
    def run(client: DistingClient) -> int:
        slot = find_ott_slot(client)
        _, name = client.algorithm_guid(slot)
        print(f"Resetting band/xover params for OTT slot={slot} ({name})")
        for i in BAND_INDICES:
            sysex_idx = i + 1
            client.set_parameter_value(slot, sysex_idx, OTT_PARAM_DEFAULTS[i])
            print(f"  [{i:2d}] {OTT_PARAM_NAMES[i]:<20s} = {OTT_PARAM_DEFAULTS[i]}")
        print("Done — routing and Global/Out/Wet unchanged. Save preset manually.")
        return 0
    return with_client(args, run)


def cmd_benchmark_vocoder(args: argparse.Namespace) -> int:
    def run(client: DistingClient) -> int:
        preset_paths = client.get_paths()
        preset_path = preset_paths[0] if preset_paths else ""
        slot = args.slot if args.slot is not None else find_vocoder_slot(client)
        guid, name = client.algorithm_guid(slot)
        if guid != VOCODER_GUID:
            raise RuntimeError(f"Slot {slot} is {guid!r} ({name}), not VOC2")

        original = client.parameter_value(slot, VOCODER_PARAM_BAND_COUNT)
        try:
            print(f"slot={slot} name={name} original_bands={original}")
            for band_count in args.band_counts:
                if preset_path:
                    client.load_preset(preset_path)
                    time.sleep(args.reload_settle)
                    slot = args.slot if args.slot is not None else find_vocoder_slot(client)
                client.set_parameter_value(slot, VOCODER_PARAM_BAND_COUNT, band_count)
                time.sleep(args.settle)
                samples = []
                for _ in range(args.samples):
                    alg, whole, _ = client.cpu_usage()
                    samples.append((alg, whole))
                    time.sleep(args.interval)
                alg_values = [s[0] for s in samples]
                whole_values = [s[1] for s in samples]
                overloaded = max(alg_values) >= 100 or max(whole_values) >= 100
                muted = max(alg_values) == 0 and max(whole_values) <= 2
                status = []
                if overloaded:
                    status.append("OVERLOAD")
                if muted:
                    status.append("MUTED")
                suffix = f" [{' '.join(status)}]" if status else ""
                print(
                    f"bands={band_count:2d} "
                    f"alg_avg={sum(alg_values)/len(alg_values):5.1f}% "
                    f"alg_max={max(alg_values):3d}% "
                    f"module_avg={sum(whole_values)/len(whole_values):5.1f}% "
                    f"module_max={max(whole_values):3d}%"
                    f"{suffix}"
                )
        finally:
            if preset_path:
                client.load_preset(preset_path)
                time.sleep(args.reload_settle)
                slot = args.slot if args.slot is not None else find_vocoder_slot(client)
            client.set_parameter_value(slot, VOCODER_PARAM_BAND_COUNT, original)
        return 0
    return with_client(args, run)


def motion_value(step_index: int, total_steps: int, lo: int, hi: int) -> int:
    if total_steps <= 1 or hi <= lo:
      return lo
    phase = step_index % (2 * (total_steps - 1))
    if phase >= total_steps:
        phase = 2 * (total_steps - 1) - phase
    return lo + ((hi - lo) * phase) // (total_steps - 1)


def cmd_benchmark_vocoder_motion(args: argparse.Namespace) -> int:
    def run(client: DistingClient) -> int:
        preset_paths = client.get_paths()
        preset_path = preset_paths[0] if preset_paths else ""
        slot = args.slot if args.slot is not None else find_vocoder_slot(client)
        guid, name = client.algorithm_guid(slot)
        if guid != VOCODER_GUID:
            raise RuntimeError(f"Slot {slot} is {guid!r} ({name}), not VOC2")

        original_params = client.all_parameter_values(slot)
        if len(original_params) <= VOCODER_PARAM_FORMANT:
            raise RuntimeError("Could not read vocoder parameters for motion benchmark")

        try:
            print(
                f"slot={slot} name={name} "
                f"bandwidth={original_params[VOCODER_PARAM_BAND_WIDTH]} "
                f"formant={original_params[VOCODER_PARAM_FORMANT]}"
            )
            for band_count in args.band_counts:
                if preset_path:
                    client.load_preset(preset_path)
                    time.sleep(args.reload_settle)
                    slot = args.slot if args.slot is not None else find_vocoder_slot(client)
                client.set_parameter_value(slot, VOCODER_PARAM_BAND_COUNT, band_count)
                time.sleep(args.settle)
                samples = []
                for sample_index in range(args.samples):
                    width = motion_value(
                        sample_index, args.motion_steps, args.width_min, args.width_max
                    )
                    formant = motion_value(
                        sample_index + args.formant_phase,
                        args.motion_steps,
                        args.formant_min,
                        args.formant_max,
                    )
                    client.set_parameter_value(slot, VOCODER_PARAM_BAND_WIDTH, width)
                    client.set_parameter_value(slot, VOCODER_PARAM_FORMANT, formant)
                    time.sleep(args.control_settle)
                    alg, whole, _ = client.cpu_usage()
                    samples.append((alg, whole, width, formant))
                    time.sleep(args.interval)
                alg_values = [s[0] for s in samples]
                whole_values = [s[1] for s in samples]
                overloaded = max(alg_values) >= 100 or max(whole_values) >= 100
                suffix = " [OVERLOAD]" if overloaded else ""
                print(
                    f"bands={band_count:2d} "
                    f"alg_avg={sum(alg_values)/len(alg_values):5.1f}% "
                    f"alg_max={max(alg_values):3d}% "
                    f"module_avg={sum(whole_values)/len(whole_values):5.1f}% "
                    f"module_max={max(whole_values):3d}%"
                    f"{suffix}"
                )
        finally:
            if preset_path:
                client.load_preset(preset_path)
                time.sleep(args.reload_settle)
                slot = args.slot if args.slot is not None else find_vocoder_slot(client)
            if original_params:
                client.set_parameter_value(
                    slot, VOCODER_PARAM_BAND_COUNT, original_params[VOCODER_PARAM_BAND_COUNT]
                )
                client.set_parameter_value(
                    slot, VOCODER_PARAM_BAND_WIDTH, original_params[VOCODER_PARAM_BAND_WIDTH]
                )
                client.set_parameter_value(
                    slot, VOCODER_PARAM_FORMANT, original_params[VOCODER_PARAM_FORMANT]
                )
        return 0
    return with_client(args, run)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Control a connected disting NT over MIDI SysEx")
    parser.add_argument("--sys-ex-id", type=int, default=0)
    parser.add_argument("--port-hint", default="disting NT")
    parser.add_argument("--timeout", type=float, default=2.0)

    sub = parser.add_subparsers(dest="cmd", required=True)

    ports = sub.add_parser("ports")
    ports.set_defaults(func=cmd_ports)

    version = sub.add_parser("version")
    version.set_defaults(func=cmd_version)

    paths = sub.add_parser("paths")
    paths.set_defaults(func=cmd_paths)

    listing = sub.add_parser("list")
    listing.add_argument("path")
    listing.set_defaults(func=cmd_list)

    cpu = sub.add_parser("cpu")
    cpu.add_argument("--samples", type=int, default=5)
    cpu.add_argument("--interval", type=float, default=0.5)
    cpu.set_defaults(func=cmd_cpu)

    push = sub.add_parser("push-plugin")
    push.add_argument("local_plugin")
    push.add_argument("--save-as", default="codex-dev")
    push.set_defaults(func=cmd_push_plugin)

    delete = sub.add_parser("delete")
    delete.add_argument("nt_path")
    delete.set_defaults(func=cmd_delete)

    dump_ott = sub.add_parser("dump-ott")
    dump_ott.set_defaults(func=cmd_dump_ott)

    init_ott = sub.add_parser("init-ott")
    init_ott.set_defaults(func=cmd_init_ott)

    reset_bands = sub.add_parser("reset-bands-ott",
                                  description="Reset band/xover params to defaults; "
                                              "leaves routing and Global/Out/Wet alone.")
    reset_bands.set_defaults(func=cmd_reset_bands_ott)

    bench = sub.add_parser("benchmark-vocoder")
    bench.add_argument("--slot", type=int)
    bench.add_argument("--samples", type=int, default=6)
    bench.add_argument("--interval", type=float, default=0.5)
    bench.add_argument("--settle", type=float, default=1.0)
    bench.add_argument("--reload-settle", type=float, default=1.0)
    bench.add_argument("--band-counts", type=lambda s: [int(x) for x in s.split(",")],
                       default=[4, 8, 12, 16, 24, 32, 40])
    bench.set_defaults(func=cmd_benchmark_vocoder)

    motion = sub.add_parser("benchmark-vocoder-motion")
    motion.add_argument("--slot", type=int)
    motion.add_argument("--samples", type=int, default=12)
    motion.add_argument("--interval", type=float, default=0.25)
    motion.add_argument("--settle", type=float, default=1.0)
    motion.add_argument("--reload-settle", type=float, default=1.0)
    motion.add_argument("--control-settle", type=float, default=0.05)
    motion.add_argument("--motion-steps", type=int, default=6)
    motion.add_argument("--width-min", type=int, default=15)
    motion.add_argument("--width-max", type=int, default=85)
    motion.add_argument("--formant-min", type=int, default=-180)
    motion.add_argument("--formant-max", type=int, default=180)
    motion.add_argument("--formant-phase", type=int, default=3)
    motion.add_argument("--band-counts", type=lambda s: [int(x) for x in s.split(",")],
                        default=[8, 12, 16, 20, 24, 28, 32])
    motion.set_defaults(func=cmd_benchmark_vocoder_motion)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        return args.func(args)
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
