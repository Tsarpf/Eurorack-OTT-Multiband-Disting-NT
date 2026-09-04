#!/usr/bin/env python3
"""Build transparent, unfrozen Max for Live devices for the Live Probe bridge."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
from pathlib import Path


def host_path(path: Path) -> str:
    value = str(path.resolve())
    if value.startswith('/mnt/') and len(value) > 6 and value[6] == '/':
        return value[5].upper() + ':' + value[6:]
    return value.replace('\\', '/')


def patcher(role: str, workspace: Path) -> dict:
    endpoint = host_path(workspace / 'bridge' / role)
    script = host_path(workspace / 'devices' / 'bridge.js')
    token = hashlib.sha256(host_path(workspace).encode()).hexdigest()[:12]
    buffer_name = 'live_probe_' + token
    boxes, lines = [], []

    def box(name, text, x, y, *, kind='newobj', **extra):
        data = dict(id=name, maxclass=kind, patching_rect=[x, y, 300, 22], **extra)
        if text is not None:
            data['text'] = text
        boxes.append({'box': data})

    def connect(source, outlet, dest, inlet=0):
        lines.append({'patchline': {'source': [source, outlet], 'destination': [dest, inlet]}})

    title = 'Live Probe ' + role.title()
    box('title', title, 15, 10, kind='comment', fontsize=18, presentation=1,
        presentation_rect=[12, 8, 260, 26])
    description = ('Plays the script-selected WAV into the next device.' if role == 'source'
                   else 'Records incoming stereo audio. Speaker output is silent.')
    box('description', description, 15, 40, kind='comment', presentation=1,
        presentation_rect=[12, 40, 275, 42], linecount=2)
    box('status', 'Waiting for Live…', 15, 85, kind='comment', varname='status_label',
        presentation=1, presentation_rect=[12, 90, 275, 50], linecount=3)
    box('js', f'js "{script}" {role} "{endpoint}" {buffer_name}', 15, 230, varname='bridge')
    box('ready', 'live.thisdevice', 15, 160)
    box('defer', 'deferlow', 15, 190)
    box('init', 'init', 330, 190, kind='message')
    connect('ready', 0, 'defer'); connect('defer', 0, 'init'); connect('init', 0, 'js')
    box('dspstate', 'dspstate~', 15, 280, varname='dsp_state')
    box('sr', 'prepend samplerate', 330, 310)
    box('dsp', 'prepend dsp', 15, 310)
    connect('dspstate', 0, 'dsp'); connect('dspstate', 1, 'sr')
    connect('dsp', 0, 'js'); connect('sr', 0, 'js')
    box('output', 'plugout~', 15, 570)
    if role == 'source':
        box('buffer', f'buffer~ {buffer_name} 1 2', 15, 370, varname='input_buffer')
        box('loaded', 'loaded', 330, 400, kind='message')
        connect('buffer', 1, 'loaded'); connect('loaded', 0, 'js')
        box('player', f'play~ {buffer_name} 2', 15, 450, varname='player')
        box('finished', 'finished', 330, 490, kind='message')
        connect('player', 2, 'finished'); connect('finished', 0, 'js')
        connect('player', 0, 'output', 0); connect('player', 1, 'output', 1)
    else:
        box('input', 'plugin~', 15, 370)
        box('recorder', 'sfrecord~ 2', 15, 450, varname='recorder')
        connect('input', 0, 'recorder', 0); connect('input', 1, 'recorder', 1)
        box('silent', 'sig~ 0', 330, 530)
        connect('silent', 0, 'output', 0); connect('silent', 0, 'output', 1)
    return {'patcher': {
        'fileversion': 1, 'appversion': {'major': 8, 'minor': 6, 'revision': 0, 'architecture': 'x64', 'modernui': 1},
        'classnamespace': 'box', 'rect': [100, 100, 760, 650], 'openrect': [0, 0, 300, 169],
        'openinpresentation': 1, 'devicewidth': 300, 'default_fontsize': 12,
        'default_fontname': 'Arial', 'boxes': boxes, 'lines': lines,
        'dependency_cache': [{'name': 'bridge.js', 'bootpath': host_path(workspace / 'devices'), 'type': 'TEXT', 'implicit': 1}],
        'latency': 0, 'autosave': 0,
        'project': {'version': 1, 'amxdtype': 1633771873, 'readonly': 0, 'devpathtype': 0, 'devpath': '.',
                    'autoorganize': 1, 'hideprojectwindow': 1, 'autolocalize': 0,
                    'contents': {'patchers': {}}, 'searchpath': {}}
    }}


def build(workspace: Path) -> list[Path]:
    workspace = workspace.resolve()
    devices = workspace / 'devices'
    devices.mkdir(parents=True, exist_ok=True)
    shutil.copy2(Path(__file__).parent / 'devices' / 'bridge.js', devices / 'bridge.js')
    paths = []
    for role in ('source', 'capture'):
        (workspace / 'bridge' / role).mkdir(parents=True, exist_ok=True)
        patch = patcher(role, workspace)
        encoded = (json.dumps(patch, ensure_ascii=True, indent=2) + '\n').encode('utf-8') + b'\0'
        container = b'ampf' + struct.pack('<I', 4) + b'aaaa' + b'meta' + struct.pack('<II', 4, 0)
        container += b'ptch' + struct.pack('<I', len(encoded)) + encoded
        path = devices / ('Live Probe ' + role.title() + '.amxd')
        path.write_bytes(container)
        path.with_suffix('.maxpat').write_text(json.dumps(patch, indent=2) + '\n')
        paths.append(path)
    (workspace / 'workspace.json').write_text(json.dumps({
        'protocol': 1, 'host_workspace': host_path(workspace),
        'bridge': host_path(workspace / 'bridge'), 'devices': [host_path(p) for p in paths],
    }, indent=2) + '\n')
    return paths


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True, help='Directory visible to both Python and Live (under /mnt/c on WSL).')
    args = parser.parse_args()
    for path in build(args.workspace):
        print(path)
