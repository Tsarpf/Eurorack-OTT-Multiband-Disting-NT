"""Deterministic stereo WAV probes, using only the Python standard library."""

from __future__ import annotations

import math
import random
import shutil
import struct
import wave
from pathlib import Path
from typing import Any, Iterable

from .bridge import local_path


def finite_number(value: Any, label: str, *, minimum: float | None = None,
                  maximum: float | None = None) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{label} must be a number")
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"{label} must be finite")
    if minimum is not None and result < minimum:
        raise ValueError(f"{label} must be >= {minimum}")
    if maximum is not None and result > maximum:
        raise ValueError(f"{label} must be <= {maximum}")
    return result


def wav_info(path: Path) -> dict[str, Any]:
    """Read PCM/IEEE float WAV metadata, including Max's float recordings."""
    size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(12)
        if len(header) != 12 or header[:4] != b"RIFF" or header[8:] != b"WAVE":
            raise ValueError(f"Not a RIFF/WAVE file: {path}")
        fmt = None
        data_bytes = None
        while stream.tell() + 8 <= size:
            chunk, length = struct.unpack("<4sI", stream.read(8))
            offset = stream.tell()
            if offset + length > size:
                raise ValueError(f"Incomplete WAV chunk in {path}")
            if chunk == b"fmt ":
                if length < 16:
                    raise ValueError(f"Invalid WAV format chunk: {path}")
                raw = stream.read(min(length, 40))
                encoding, channels, sample_rate, _, align, bits = struct.unpack("<HHIIHH", raw[:16])
                if encoding == 0xFFFE and len(raw) >= 40:
                    encoding = struct.unpack("<H", raw[24:26])[0]
                fmt = (encoding, channels, sample_rate, align, bits)
            elif chunk == b"data":
                data_bytes = length
            stream.seek(offset + length + (length & 1))
        if fmt is None or data_bytes is None:
            raise ValueError(f"WAV is missing format or audio data: {path}")
        encoding, channels, sample_rate, align, bits = fmt
        if encoding not in (1, 3) or channels not in (1, 2):
            raise ValueError(f"Only mono/stereo PCM or float WAV is supported: {path}")
        if sample_rate <= 0 or align <= 0 or data_bytes % align:
            raise ValueError(f"Invalid WAV frame layout: {path}")
        frames = data_bytes // align
        return {"channels": channels, "sample_rate": sample_rate,
                "frames": frames, "duration_seconds": frames / sample_rate,
                "bits_per_sample": bits, "encoding": "float" if encoding == 3 else "pcm"}


def wav_statistics(path: Path) -> dict[str, Any]:
    """Measure all recorded samples, including preroll/tail, and reject NaNs."""
    info = wav_info(path)
    channels = info["channels"]
    bits = info["bits_per_sample"]
    encoding = info["encoding"]
    if (encoding == "pcm" and bits not in (8, 16, 24, 32)) or (encoding == "float" and bits not in (32, 64)):
        raise ValueError(f"Unsupported sample format for measurement: {encoding}{bits}")
    peaks = [0.0] * channels
    squares = [0.0] * channels
    nonfinite = [0] * channels
    above_full_scale = [0] * channels
    sample_width = bits // 8
    frame_width = sample_width * channels
    total_frames = 0
    with path.open("rb") as stream:
        stream.seek(12)
        while True:
            header = stream.read(8)
            if len(header) != 8:
                break
            chunk, length = struct.unpack("<4sI", header)
            offset = stream.tell()
            if chunk != b"data":
                stream.seek(offset + length + (length & 1))
                continue
            remaining = length
            while remaining:
                data = stream.read(min(4096 * frame_width, remaining))
                remaining -= len(data)
                if not data or len(data) % frame_width:
                    raise ValueError(f"Incomplete audio samples in {path}")
                total_frames += len(data) // frame_width
                if encoding == "float":
                    format_code = "<f" if bits == 32 else "<d"
                    samples = (value[0] for value in struct.iter_unpack(format_code, data))
                elif bits == 8:
                    samples = ((value - 128) / 128 for value in data)
                else:
                    divisor = float(1 << (bits - 1))
                    samples = (int.from_bytes(data[index:index + sample_width], "little", signed=True) / divisor
                               for index in range(0, len(data), sample_width))
                for index, value in enumerate(samples):
                    channel = index % channels
                    if not math.isfinite(value):
                        nonfinite[channel] += 1
                        continue
                    absolute = abs(value)
                    peaks[channel] = max(peaks[channel], absolute)
                    squares[channel] += value * value
                    if absolute > 1:
                        above_full_scale[channel] += 1
            # Only one data chunk is supported by the playback/recording path.
            break
    if any(nonfinite):
        raise ValueError(f"Non-finite samples in {path}: {nonfinite} per channel")
    if total_frames != info["frames"]:
        raise ValueError(f"WAV frame count changed during analysis: {path}")
    result = []
    for channel in range(channels):
        peak = peaks[channel]
        rms = math.sqrt(squares[channel] / total_frames) if total_frames else 0.0
        if not math.isfinite(rms):
            raise ValueError(f"Audio magnitude exceeds supported range in {path}")
        result.append({"peak": peak, "peak_dbfs": 20 * math.log10(peak) if peak else None,
                       "rms": rms, "rms_dbfs": 20 * math.log10(rms) if rms else None,
                       "above_full_scale_samples": above_full_scale[channel]})
    return {"channels": result, "silent": not any(peaks), "nonfinite_samples": 0,
            "window": "entire recording, including preroll and tail"}


def _level(value: Any, label: str = "level_dbfs") -> float:
    return 10 ** (finite_number(value, label, minimum=-180, maximum=0) / 20)


def _frequency(value: Any, sample_rate: int) -> float:
    value = finite_number(value, "frequency", minimum=0.001)
    if value >= sample_rate / 2:
        raise ValueError("frequency must be below the Nyquist frequency")
    return value


def _write_pcm24(path: Path, samples: Iterable[tuple[float, float]], sample_rate: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(3)
        stream.setframerate(sample_rate)
        block = bytearray()
        for pair in samples:
            for value in pair:
                integer = max(-8388608, min(8388607, round(value * 8388607)))
                block.extend(integer.to_bytes(3, "little", signed=True))
            if len(block) >= 49152:
                stream.writeframesraw(block)
                block.clear()
        if block:
            stream.writeframesraw(block)


def write_signal(spec: dict[str, Any], path: Path, sample_rate: int,
                 manifest_directory: Path) -> dict[str, Any]:
    kind = spec.get("kind")
    allowed = {"name", "kind"}
    if kind == "wav":
        allowed.add("path")
    elif kind == "silence":
        allowed.add("duration")
    elif kind == "stepped_sine":
        allowed.update({"frequency", "levels_dbfs", "segment_seconds", "right_gain_db"})
    else:
        allowed.update({"level_dbfs", "right_gain_db", "duration"})
        allowed.update({"sine": {"frequency"}, "impulse": {"offset_seconds"}, "noise": {"seed"},
                        "sweep": {"start_frequency", "end_frequency"}}.get(kind, set()))
    unknown = set(spec) - allowed
    if unknown:
        raise ValueError(f"Unknown {kind} signal fields: {', '.join(sorted(unknown))}")
    if kind == "wav":
        if not isinstance(spec.get("path"), str):
            raise ValueError("wav signal requires a path")
        source = local_path(spec["path"])
        if not source.is_absolute():
            source = manifest_directory / source
        info = wav_info(source)
        if not info["frames"]:
            raise ValueError(f"Input WAV is empty: {source}")
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, path)
        return {**info, "source_path": str(source.resolve())}
    if kind not in {"sine", "stepped_sine", "impulse", "noise", "sweep", "silence"}:
        raise ValueError(f"Unknown signal kind: {kind!r}")
    sample_rate = int(finite_number(sample_rate, "sample_rate", minimum=8000, maximum=192000))
    level = _level(spec.get("level_dbfs", -18))
    right_gain = _level(spec.get("right_gain_db", 0), "right_gain_db")
    duration = finite_number(spec.get("duration", 2.0), "duration", minimum=0.001, maximum=3600)
    frequency = None
    if kind in {"sine", "stepped_sine"}:
        frequency = _frequency(spec.get("frequency", 440), sample_rate)
    if kind == "stepped_sine":
        levels = spec.get("levels_dbfs", [-48, -36, -24, -12, -6])
        if not isinstance(levels, list) or not levels:
            raise ValueError("levels_dbfs must be a nonempty list")
        amplitudes = [_level(value, "levels_dbfs value") for value in levels]
        segment = finite_number(spec.get("segment_seconds", 1.0), "segment_seconds",
                                minimum=0.001, maximum=3600)
        segment_frames = round(segment * sample_rate)
        frames = segment_frames * len(levels)
        if frames / sample_rate > 3600:
            raise ValueError("Signal may not exceed one hour")
    else:
        frames = max(1, round(duration * sample_rate))
    if kind == "impulse":
        offset = finite_number(spec.get("offset_seconds", 0.1), "offset_seconds", minimum=0)
        impulse_frame = round(offset * sample_rate)
        if impulse_frame >= frames:
            raise ValueError("impulse offset must occur within duration")
    if kind == "noise":
        seed = spec.get("seed", 0)
        if not isinstance(seed, int) or isinstance(seed, bool):
            raise ValueError("noise seed must be an integer")
        random_source = random.Random(seed)
    if kind == "sweep":
        start = _frequency(spec.get("start_frequency", 20), sample_rate)
        end = _frequency(spec.get("end_frequency", 20000), sample_rate)
        if end <= start:
            raise ValueError("end_frequency must exceed start_frequency")
        log_ratio = math.log(end / start)

    def samples() -> Iterable[tuple[float, float]]:
        for index in range(frames):
            seconds = index / sample_rate
            if kind == "sine":
                value = level * math.sin(2 * math.pi * frequency * seconds)
            elif kind == "stepped_sine":
                amplitude = amplitudes[min(index // segment_frames, len(amplitudes) - 1)]
                value = amplitude * math.sin(2 * math.pi * frequency * seconds)
            elif kind == "impulse":
                value = level if index == impulse_frame else 0.0
            elif kind == "noise":
                # Uniform white noise: level_dbfs is the peak ceiling, with
                # RMS approximately 4.77 dB below it.
                value = level * random_source.uniform(-1, 1)
            elif kind == "sweep":
                phase = 2 * math.pi * start * duration / log_ratio * math.expm1(seconds / duration * log_ratio)
                value = level * math.sin(phase)
            else:
                value = 0.0
            yield value, value * right_gain

    _write_pcm24(path, samples(), sample_rate)
    return wav_info(path)
