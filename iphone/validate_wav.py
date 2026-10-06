#!/usr/bin/env python3
"""Validate the float WAV artifacts produced by the on-device tests."""

import math
import struct
import sys
from pathlib import Path


def validate(path: Path) -> None:
    blob = path.read_bytes()
    if len(blob) < 12 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")

    fmt = None
    audio = None
    offset = 12
    while offset + 8 <= len(blob):
        chunk_id = blob[offset : offset + 4]
        chunk_size = struct.unpack_from("<I", blob, offset + 4)[0]
        start = offset + 8
        end = start + chunk_size
        if end > len(blob):
            raise ValueError("truncated chunk")
        if chunk_id == b"fmt ":
            if chunk_size < 16:
                raise ValueError("short fmt chunk")
            fmt = struct.unpack_from("<HHIIHH", blob, start)
        elif chunk_id == b"data":
            audio = blob[start:end]
        offset = end + (chunk_size & 1)

    if fmt is None or audio is None:
        raise ValueError("missing fmt or data chunk")
    tag, channels, rate, _byte_rate, block_align, bits = fmt
    if (tag, channels, rate, block_align, bits) != (3, 2, 48000, 8, 32):
        raise ValueError(
            f"unexpected format tag={tag} channels={channels} rate={rate} "
            f"block_align={block_align} bits={bits}"
        )
    if len(audio) == 0 or len(audio) % block_align:
        raise ValueError("empty or unaligned data chunk")

    samples = struct.unpack(f"<{len(audio) // 4}f", audio)
    finite = [sample for sample in samples if math.isfinite(sample)]
    if len(finite) != len(samples):
        raise ValueError("non-finite sample")
    rms = math.sqrt(sum(sample * sample for sample in samples) / len(samples))
    peak = max(abs(sample) for sample in samples)
    if rms <= 1.0e-7 or peak <= 1.0e-7:
        raise ValueError("silent audio")
    print(
        f"validated {path}: frames={len(samples) // channels} "
        f"finite={len(finite)}/{len(samples)} rms={rms:.9f} peak={peak:.9f}"
    )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} FILE", file=sys.stderr)
        raise SystemExit(64)
    try:
        validate(Path(sys.argv[1]))
    except (OSError, ValueError, struct.error) as error:
        print(f"WAV validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
