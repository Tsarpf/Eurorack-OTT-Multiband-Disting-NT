#!/usr/bin/env python3
"""Capture/analyze Vocoder carrier normalization through the reusable Live bridge.

Live must contain the 40-band, Modulator-carrier Vocoder Probe set. Precise is
selected through the inverse-labelled Precise/Retro API switch. Live's upper
limit is 18 kHz; the native plugin independently supports 20 kHz.

Run with --capture to prepare and record; omit it to re-analyze completed runs.
Requires NumPy, SciPy, SoundFile, and a built vocoder/build/render_probe.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys

import numpy as np
import soundfile as sf
from scipy.signal import welch

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.live_probe.bridge import atomic_json
from tools.live_probe.runner import prepare_run, run_batch

SR = 48000
FREQUENCIES = [40, 100, 300, 1000, 3000, 6000, 12000, 16000]


def read(path):
    return json.loads(path.read_text())


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    parts = [np.zeros(SR // 2)]
    tones = []
    for frequency in FREQUENCIES:
        for level in [-42, -18, -6]:
            start = sum(map(len, parts))
            parts.append((np.sin(2 * np.pi * frequency * (np.arange(SR) / SR)) *
                          10 ** (level / 20)).astype('float32'))
            tones.append(dict(frequency=frequency, level=level, start=start, end=start + SR))
    signal = np.concatenate(parts + [np.zeros(SR // 2)])
    sf.write(root / 'tones.wav', np.column_stack([signal, signal]), SR, subtype='FLOAT')
    rng = np.random.default_rng(20260905)
    parts = [np.zeros(SR // 2)]
    noise = []
    for color in ['white', 'pink', 'brown']:
        spectrum = np.fft.rfft(rng.normal(size=SR * 3))
        frequency = np.maximum(np.fft.rfftfreq(SR * 3, 1 / SR), 20)
        if color != 'white':
            spectrum /= frequency ** (.5 if color == 'pink' else 1)
        signal = np.fft.irfft(spectrum, n=SR * 3)
        signal *= .15 / np.max(abs(signal))
        start = sum(map(len, parts))
        noise.append(dict(color=color, start=start, end=start + len(signal)))
        parts.append(signal)
    signal = np.concatenate(parts + [np.zeros(SR // 2)])
    sf.write(root / 'noise.wav', np.column_stack([signal, signal]), SR, subtype='FLOAT')
    atomic_json(root / 'segments.json', dict(tones=tones, noise=noise))
    fixed = {'Device On': 'On', 'Dry/Wet': 1, 'Output': 0, 'Precise/Retro': 'Off',
             'Unvoiced Level': 0, 'Mono/Stereo': 'Mono', 'Gate Threshold': -60,
             'Lower Filter Band': math.log10(20), 'Upper Filter Band': {'normalized': 1},
             'Filter Width': 1, 'Envelope Depth': 0, 'Formant Shift': 0,
             'Attack Time': 1, 'Release Time': math.log10(30)}
    manifest = dict(version=1, name='Enhance identification', device='Filterbank',
                    sample_rate=SR, settle_seconds=1, preroll_seconds=.25, tail_seconds=.5,
                    parameters=fixed, sweep={'Enhance': ['Off', 'On']},
                    signals=[dict(name=n, kind='wav', path=str(root / (n + '.wav')))
                             for n in ['tones', 'noise']])
    atomic_json(root / 'manifest.json', manifest)
    manifest['parameters'] = {**fixed, 'Enhance': 'On'}
    manifest['sweep'] = {'Filter Width': [.5, 1, 2], 'Envelope Depth': [0, 1]}
    manifest['signals'] = manifest['signals'][1:]
    atomic_json(root / 'validate-manifest.json', manifest)


def capture(root, bridge):
    if (root / 'run.json').exists() or (root / 'validate-run.json').exists():
        raise ValueError('Use a fresh --root for a new capture; existing runs are retained.')
    prepare(root)
    for prefix in ['', 'validate-']:
        directory, plan = prepare_run(root / (prefix + 'manifest.json'), root / 'runs')
        atomic_json(root / (prefix + 'run.json'), {'directory': str(directory)})
        summary = run_batch(bridge, directory, plan,
                            progress=lambda x: print(x.split(' {')[0], flush=True))
        if summary['status'] != 'complete':
            raise RuntimeError(f'Capture incomplete: {summary}')


def audio(path):
    samples, rate = sf.read(path, always_2d=True)
    if rate != SR or not np.isfinite(samples).all():
        raise ValueError(f'Invalid capture: {path}')
    return samples[:, 0]


def window(samples, segment, offset, guard):
    return samples[int(segment['start'] + SR * (offset + guard)):
                   int(segment['end'] + SR * (offset - .1))]


def analyze(root, renderer):
    native = root / 'native-enhance'
    native.mkdir(exist_ok=True)
    segments = read(root / 'segments.json')
    report = dict(renderer_sha256=digest(renderer), sample_rate=SR,
                  bus_volts_per_full_scale=5, bands=40, range_hz=[20, 18000],
                  spectral_centers_hz=FREQUENCIES,
                  alignment='Live first sample above 1e-6 minus 0.5s input silence; '
                            'native metadata. Steady windows only, not latency measurements.',
                  inputs={n: digest(root / (n + '.wav')) for n in ['tones', 'noise']}, cases=[])
    for prefix in ['', 'validate-']:
        run = Path(read(root / (prefix + 'run.json'))['directory'])
        for case in sorted((run / 'cases').iterdir()):
            metadata = read(case / 'metadata.json')
            if metadata['status'] != 'complete':
                raise ValueError(f'Incomplete case: {case}')
            p = metadata['case']['parameters']
            width, depth = round(p['Filter Width'] * 100), round(p['Envelope Depth'] * 100)
            enhance = int(p['Enhance'] == 'On')
            name = metadata['input']['name']
            stem = native / f'{name}-w{width}-d{depth}-e{enhance}'
            command = [str(renderer), '--input', str(root / (name + '.wav')),
                       '--output', str(stem.with_suffix('.wav')), '--metadata', str(stem.with_suffix('.json')),
                       '--width', str(width), '--depth', str(depth), '--enhance', str(enhance), '--min-hz', '20']
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
            live = audio(case / 'output.wav')
            active = np.flatnonzero(abs(live) > 1e-6)
            if not len(active):
                raise ValueError(f'Silent capture: {case}')
            offsets = [active[0] / SR - .5, read(stem.with_suffix('.json'))['input_start_frame'] / SR]
            candidate = audio(stem.with_suffix('.wav'))
            row = dict(case=str(case), reference_sha256=digest(case / 'output.wav'),
                       native_sha256=digest(stem.with_suffix('.wav')), width=width,
                       depth=depth, enhance=enhance, signal=name, offsets_seconds=offsets, measurements=[])
            for segment in segments[name]:
                waves = [window(x, segment, o, .4 if name == 'tones' else 1)
                         for x, o in zip([live, candidate], offsets)]
                levels = [20 * np.log10(max(np.sqrt(np.mean(x * x)), 1e-20)) for x in waves]
                measurement = dict(segment=segment, live_rms_dbfs=levels[0], native_rms_dbfs=levels[1],
                                   level_error_db=levels[1] - levels[0])
                if name == 'noise':
                    ps = [welch(x, SR, nperseg=8192) for x in waves]
                    f = ps[0][0]
                    delta = 10 * np.log10(np.maximum(ps[1][1], 1e-30) / np.maximum(ps[0][1], 1e-30))
                    measurement['spectral_error_db'] = [float(np.mean(delta[(f > fc / 1.15) & (f < fc * 1.15)]))
                                                        for fc in FREQUENCIES]
                row['measurements'].append(measurement)
            report['cases'].append(row)
    atomic_json(root / 'enhance-comparison.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--renderer', type=Path, default=Path(__file__).resolve().parents[2] / 'vocoder/build/render_probe')
    args = parser.parse_args()
    root = args.root.resolve()
    if args.capture:
        if args.bridge is None:
            parser.error('--capture requires --bridge')
        capture(root, args.bridge.resolve())
    report = analyze(root, args.renderer.resolve())
    print(f'Analyzed {len(report["cases"])} cases: {root / "enhance-comparison.json"}')


if __name__ == '__main__':
    main()
