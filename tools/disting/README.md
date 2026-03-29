# Disting NT Host Tools

These scripts talk to a connected disting NT over MIDI SysEx.

Setup:

```bash
python3 -m venv .venv-disting
. .venv-disting/bin/activate
pip install -r tools/disting/requirements.txt
```

Examples:

```bash
. .venv-disting/bin/activate
python tools/disting/ntctl.py version
python tools/disting/ntctl.py paths
python tools/disting/ntctl.py list /programs/plug-ins
python tools/disting/ntctl.py push-plugin vocoder/vocoder.o --save-as codex-dev
python tools/disting/ntctl.py benchmark-vocoder --samples 8 --interval 0.5
```

Notes:

- Default SysEx ID is `0`.
- The benchmark command finds the current `VOC2` plug-in slot unless `--slot` is given.
- CPU output reports both percentages returned by the module: algorithm CPU and whole-module CPU.
