#!/usr/bin/env python3
"""Measure low Formant at Width50/75/100 and Depth100/150/200.

Live must have the 40-band Modulator-carrier probe project open. All cases use
Enhance On, Precise, 20 Hz–18 kHz, Attack10/Release30 ms. No DSP files are edited.
Prepare/render with --native-only; record/resume with --capture --bridge PATH.
"""
from __future__ import annotations
import argparse
import itertools
import math
from pathlib import Path
import subprocess
import sys
import numpy as np
import soundfile as sf
from scipy.signal import welch, butter, sosfilt, correlate, correlation_lags

if __package__ in {None, ''}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.live_probe.bridge import atomic_json, read_json
from tools.live_probe.runner import prepare_run, resume_run, run_batch
from tools.live_probe.measure_enhance import digest

SR = 48000


def prepare(root):
    root.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20260906)
    n = 2 * SR
    t = np.arange(n) / SR
    white = rng.normal(size=n)
    white *= 10 ** (-18 / 20) / np.sqrt(np.mean(white ** 2))
    frequency = np.maximum(np.fft.rfftfreq(n, 1 / SR), 20)
    spectral_shape = .03 + sum(np.exp(-.5 * (np.log2(frequency / fc) / .22) ** 2)
                               for fc in [500, 1500, 3500])
    vowel = np.fft.irfft(np.fft.rfft(rng.normal(size=n)) * spectral_shape)
    vowel *= .5 / np.max(abs(vowel))
    # Bandlimited saw: no aliasing in the source when evaluating roughness.
    saw = sum(np.sin(2 * np.pi * 110 * k * t) / k for k in range(1, 201))
    saw /= np.max(abs(saw))
    parts = [np.zeros(SR // 2)]
    segments = []
    for name, signal in [('white', white), ('vowel', vowel),
                         ('saw_loud', saw * 10 ** (-6 / 20)),
                         ('saw_quiet', saw * 10 ** (-18 / 20))]:
        start = sum(map(len, parts))
        segments.append(dict(name=name, start=start, end=start + n))
        parts.append(signal)
    signal = np.concatenate(parts + [np.zeros(SR // 2)])
    sf.write(root / 'probe.wav', np.column_stack([signal, signal]), SR, subtype='FLOAT')
    atomic_json(root / 'segments.json', segments)
    fixed = {'Device On': 'On', 'Dry/Wet': 1, 'Output': 0, 'Enhance': 'On',
             'Precise/Retro': 'Off', 'Unvoiced Level': 0, 'Mono/Stereo': 'Mono',
             'Gate Threshold': -60, 'Lower Filter Band': math.log10(20),
             'Upper Filter Band': {'normalized': 1}, 'Attack Time': 1,
             'Release Time': math.log10(30)}
    cases = [{'Formant Shift': f, 'Filter Width': w / 100, 'Envelope Depth': d / 100}
             for f, w, d in itertools.product([0, -24, -30, -36], [50, 75, 100], [100, 150, 200])]
    manifest = dict(version=1, name='Low Formant identification', device='Filterbank',
                    sample_rate=SR, parameters=fixed, cases=cases, settle_seconds=.75,
                    preroll_seconds=.25, tail_seconds=.5,
                    signals=[dict(name='probe', kind='wav', path=str(root / 'probe.wav'))])
    atomic_json(root / 'manifest.json', manifest)
    run, plan = prepare_run(root / 'manifest.json', root / 'runs')
    atomic_json(root / 'run.json', {'directory': str(run)})
    return run, plan


def render(root, plan, renderer):
    directory = root / 'native'
    directory.mkdir(exist_ok=True)
    stamp = dict(renderer_sha256=digest(renderer), input_sha256=digest(root / 'probe.wav'),
                 cases=plan['cases'])
    stamp_path = directory / 'render-state.json'
    if stamp_path.exists() and read_json(stamp_path) == stamp and all(
            (directory / (c['id'] + suffix)).exists()
            for c in plan['cases'] for suffix in ['.wav', '.json']):
        return
    for case in plan['cases']:
        p = case['parameters']
        output = directory / (case['id'] + '.wav')
        subprocess.run([str(renderer), '--input', str(root / 'probe.wav'), '--output', str(output),
                        '--metadata', str(output.with_suffix('.json')), '--bands', '40', '--enhance', '1',
                        '--formant-semitones', str(p['Formant Shift']),
                        '--width', str(round(p['Filter Width'] * 100)),
                        '--depth', str(round(p['Envelope Depth'] * 100)),
                        '--min-hz', '20', '--max-hz', '18000'], check=True, stdout=subprocess.DEVNULL)

    atomic_json(stamp_path, stamp)

def metrics(path, segments, offset, reference_input=None, formant=0):
    audio, rate = sf.read(path, always_2d=True)
    if rate != SR or not np.isfinite(audio).all():
        raise ValueError(f'Invalid audio: {path}')
    signal = audio[:, 0]
    if offset is None:
        if reference_input is None:
            raise ValueError('Live alignment requires the original white-noise probe')
        original, _ = sf.read(reference_input, always_2d=True)
        # Correlate the deterministic white-noise section. Very low Formant
        # can leave bass tails before playback, making onset detection invalid.
        ratio = 2 ** (formant / 12)
        sos = butter(3, [4000 * ratio, 14000 * ratio], fs=SR, btype='bandpass', output='sos')
        x = sosfilt(sos, original[:int(2.5 * SR), 0])
        y = sosfilt(sos, signal[:int(3.3 * SR)])
        correlation = correlate(y, x, mode='full', method='fft')
        lags = correlation_lags(len(y), len(x))
        valid = np.flatnonzero((lags >= .15 * SR) & (lags <= .7 * SR))
        peak = valid[np.argmax(abs(correlation[valid]))]
        offset = float(lags[peak] / SR)
    result = []
    for segment in segments:
        x = signal[int(segment['start'] + SR * (offset + .8)):
                   int(segment['end'] + SR * (offset - .15))]
        f, psd = welch(x, SR, nperseg=16384)
        power = float(np.sum(psd))
        row = dict(name=segment['name'], rms_dbfs=float(20 * np.log10(max(np.sqrt(np.mean(x*x)), 1e-30))),
                   peak_dbfs=float(20 * np.log10(max(np.max(abs(x)), 1e-30))),
                   centroid_hz=float(np.sum(f * psd) / max(power, 1e-30)),
                   rolloff95_hz=float(f[min(np.searchsorted(np.cumsum(psd), .95 * power), len(f)-1)]),
                   power_fraction_above={str(hz): float(np.sum(psd[f >= hz]) / max(power, 1e-30))
                                         for hz in [2500, 4500, 6000, 10000]})
        result.append(row)
    return result, offset


def analyze(root, run, plan, renderer):
    segments = read_json(root / 'segments.json')
    rows = []
    for case in plan['cases']:
        row = dict(id=case['id'], parameters=case['parameters'])
        native = root / 'native' / (case['id'] + '.wav')
        offset = read_json(native.with_suffix('.json'))['input_start_frame'] / SR
        row['native'], row['native_offset'] = metrics(native, segments, offset)
        reference = run / case['directory'] / 'output.wav'
        if reference.exists():
            row['live'], row['live_offset'] = metrics(reference, segments, None, root / 'probe.wav', case['parameters']['Formant Shift'])
            row['reference_sha256'] = digest(reference)
        rows.append(row)
    atomic_json(root / 'comparison.json', dict(renderer_sha256=digest(renderer),
                input_sha256=digest(root / 'probe.wav'), sample_rate=SR, cases=rows))
    print(f'Analyzed {len(rows)} native cases, {sum("live" in r for r in rows)} Live cases', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    parser.add_argument('--bridge', type=Path)
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--native-only', action='store_true')
    parser.add_argument('--renderer', type=Path, default=Path(__file__).resolve().parents[2] / 'vocoder/build/render_probe')
    args = parser.parse_args()
    root, renderer = args.root.resolve(), args.renderer.resolve()
    if (root / 'run.json').exists():
        run = Path(read_json(root / 'run.json')['directory'])
        plan = resume_run(run)
    else:
        run, plan = prepare(root)
    if args.capture:
        if args.bridge is None:
            parser.error('--capture requires --bridge')
        print(run_batch(args.bridge, run, plan, progress=lambda x: print(x.split(' {')[0], flush=True)))
    render(root, plan, renderer)
    analyze(root, run, plan, renderer)

if __name__ == '__main__':
    main()
