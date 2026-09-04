# Live Probe

Run repeatable audio measurements through Ableton Live effects from Python. A
Source device plays generated or existing WAV files, and a Capture device saves
the processed audio as stereo float WAV. Python changes parameters, waits for
playback, records the tail, and advances through a batch without import/export
dialogs. Live stays open and audio runs in real time.

The capture runner uses only the standard library, with Python 3.10 or newer.
The optional native-vocoder comparison scripts also use NumPy, SciPy, SoundFile,
and Matplotlib. Live and Max for Live are required for reference captures.
Windows Live can be controlled from either Windows Python or WSL; the supplied
initial setup helper uses Windows PowerShell.

## Set up a reusable measurement set

Use a workspace on a Windows drive so both Live and Python can access it. These
commands run from the repository root in WSL:

```sh
export LIVE_PROBE_BRIDGE=/mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe/bridge

python3 tools/live_probe/setup_windows.py \
  --workspace /mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe \
  --set-name 'Live Probe Setup.als' \
  --launch
```

For this initial `--launch` step, save and close Live first. The helper prepares
an empty set from Live's installed default, opens it, then uses Live's own file
opening to insert the generated Max devices. It refuses to replace an existing
set. Add `--device-preset /path/to/effect.adv` to insert a native audio effect
between them. Installation defaults target Live 12 Suite; `--resources` and
`--live-exe` can select another installation.

With `--launch`, the helper saves the finished set automatically through Live's
native menu and verifies its device chain on disk. Reopen that set for future
batches. Automatic launch and saving are verified with Windows Live 12 Suite
using English menus. The desired device order on one audio track is:

```text
Live Probe Source → effect(s) being measured → Live Probe Capture
```

Keep the bridge device names unchanged, and use one Source/Capture pair per
workspace. The Source supplies its own audio, independent of arrangement clips
and transport playback. Capture records its input and silences the output to
the speakers. Devices inside effect racks between Source and Capture are also
discoverable.

To prepare files without launching Live, omit `--launch`. Open the generated set,
add the `.amxd` devices from the workspace's `devices` folder in the order above,
and save. For an existing measurement set, build just the devices with:

```sh
python3 tools/live_probe/build_devices.py \
  --workspace /mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe
```

Rebuild devices with the measurement set closed. Their JavaScript is watched for
changes; replacing it during a batch restarts the bridge. Keep the workspace at
the same path because the generated devices refer to it.

Windows PowerShell uses the same runner commands with `python` and a Windows
environment variable:

```powershell
$env:LIVE_PROBE_BRIDGE = 'C:\Users\tsarpf\Documents\Ableton\LiveProbe\bridge'
python -m tools.live_probe doctor
```

`--bridge PATH` can replace the environment variable; put it before the command,
for example `python3 -m tools.live_probe --bridge /mnt/c/.../bridge doctor`.

## Check the chain and inspect controls

Enable Live's audio engine and set its sample rate to the manifest's
`sample_rate`, normally 48000 Hz. The output WAV reports the actual recording
sample rate. Existing input WAVs retain their own sample rate, so choose matching
files when exact comparisons matter.

```sh
python3 -m tools.live_probe doctor
python3 -m tools.live_probe status
python3 -m tools.live_probe devices
python3 -m tools.live_probe parameters --device 'Filterbank'
```

`devices` lists effects between Source and Capture. Names must match exactly and
identify one device. A manifest accepts `"device": "Filterbank"`,
`"device": {"name": "Filterbank"}`, or `"device": {"path": "live_set tracks 0 devices 1"}`.
Use a path returned by `devices` to distinguish duplicate names. The inspection
commands also accept `--device-path` instead of `--device`.

Parameter descriptions include raw minimum/maximum/current values, display
text, discrete choices, and enabled state. Parameter mapping keys can use the
reported name, original name, or parameter path. Values support:

| JSON value | Meaning |
| --- | --- |
| `0.5` | Raw Live parameter value; units depend on the parameter. |
| `"On"` | A discrete item label from an exposed parameter. |
| `{"normalized": 0.5}` | Halfway between the reported raw minimum and maximum. |
| `{"display": "100 %"}` | Invert the numeric display to find the corresponding raw value. |

For display values, use the unit spelling returned by `parameters`. Numeric
display inversion supports monotonic controls; if a device has a special or
nonmonotonic display, use its raw or normalized values. Every change is checked
against the device's readback. A standalone `set` intentionally leaves its
changes applied:

```sh
python3 -m tools.live_probe set --device 'Filterbank' '{"Envelope Depth":{"display":"100 %"}}'
```

Pass `@values.json` instead of inline JSON when shell quoting is inconvenient.

## Vocoder in Modulator mode

[examples/vocoder.json](examples/vocoder.json) targets a **40-band** Vocoder in
**Modulator** mode, which uses the input as its own carrier. Derive the local
reference preset from Live's installed `Filterbank.adv`, then create the
measurement set with Live closed:

```sh
python3 tools/live_probe/prepare_vocoder.py \
  --output '/mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe/Vocoder Reference.adv'

python3 tools/live_probe/setup_windows.py \
  --workspace /mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe \
  --set-name 'Vocoder Probe.als' \
  --device-preset '/mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe/Vocoder Reference.adv' \
  --launch

python3 -m tools.live_probe parameters --device 'Filterbank'
python3 -m tools.live_probe run tools/live_probe/examples/vocoder.json
```

The device's API name is `Filterbank`, inherited from the preset; its class is
`Vocoder`. The depth parameter is named `Envelope Depth`, and bandwidth is
`Filter Width`. The example uses **Precise**, **Enhance on**, fully wet output,
and a 20 Hz–18 kHz frequency range. The API exposes Precise as
`"Precise/Retro": "Off"`.

The baseline is width 100%, depth 100%, formant 0, and release **30 ms**. The
example varies one control at a time through these values:

| Control | Values |
| --- | --- |
| Width | 10, 25, 50, 100, 150, 200% |
| Depth | 0, 25, 50, 100, 150, 200% |
| Formant | −36, −24, −12, 0, +12, +24, +36 |
| Release | 10, 20, 30, 40, 60, 100 ms |

One shared baseline eliminates repeated settings, leaving **22 settings × 3
probes = 66 captures**. The probes are an impulse, white noise, and a stepped
440 Hz sine with 0.5-second level segments. Edit fixed `parameters` or explicit
`cases` to change release or any other exposed control. The preset helper also
accepts `--release-ms`, with a default of 30 ms.

Live's verified limits differ from the requested width 0–200% and frequency
range 20 Hz–20 kHz: **width starts at 10%, and the upper band stops at 18 kHz**.
The example uses those actual endpoints; requests for 0% width or 20 kHz are
rejected by the device. Depth supports 0–200%, and formant supports −36–+36.

Carrier selection and band count are not exposed as automatable parameters by
this Vocoder. `prepare_vocoder.py` stores Modulator and 40 bands in the derived
preset, which is saved into the measurement set. It reads the installed source
at `C:/ProgramData/Ableton/Live 12 Suite/Resources/Core Library/Devices/Audio Effects/Vocoder/Filterbank.adv`
by default; `--resources` or `--source` can select another installation. Save
another set if you need another band count. Parameter discovery lists the
controls the API can change; it does not expose every visible control.

## Compare the native vocoder with Live

The [measured comparison](../../vocoder/fixtures/analysis/ableton_comparison/README.md)
documents the current differences in Depth, Width, Formant, and Release.
`capture_comparison.py` prepares deterministic noise, a stepped tone, and a
three-peak shaped-noise signal, then captures four focused suites. It uses the
common native/Live range of **30 Hz–18 kHz**, with 40 bands, Precise, Modulator,
attack 10 ms, release 30 ms, and identical mono channels. Depth and Formant are
measured with Enhance both on and off. Width uses Depth 0 and Enhance off.

First prepare the 40-band Modulator measurement set as above and open it with
the audio engine running at 48 kHz. Install the optional analysis dependencies
in your Python environment:

```sh
python3 -m pip install numpy scipy soundfile matplotlib
make -C vocoder render-probe

export VOCODER_COMPARISON=/mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe/comparisons/my-comparison

python3 tools/live_probe/capture_comparison.py \
  --workspace /mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe \
  --output "$VOCODER_COMPARISON"

python3 tools/live_probe/analyze_width.py --comparison "$VOCODER_COMPARISON" --render-native
python3 tools/live_probe/analyze_depth.py --comparison "$VOCODER_COMPARISON" --force-render
python3 tools/live_probe/analyze_formant.py --comparison "$VOCODER_COMPARISON" --render-native
python3 tools/live_probe/analyze_release.py --comparison "$VOCODER_COMPARISON"
python3 tools/live_probe/summarize_comparison.py --comparison "$VOCODER_COMPARISON"
```

Add `--prepare-only` to prepare signals and manifests without contacting Live.
`--suites width depth formant` selects suites; the default also includes release.
Repeating the capture command with an existing output directory resumes its
unfinished cases. Use a **new output directory for each DSP revision** to keep
the baseline evidence intact. The configuration stores DSP source hashes;
render metadata records effective controls, and analyses store audio hashes.
Only the capture command contacts Live. The analysis commands read the reference
WAVs and optionally run the native renderer; they produce JSON metrics, PNG
figures, and spectral arrays under `analysis/`.

Run Width analysis before Formant: its best broad-width shape match is included
in the Formant grid. Native rendering is opt-in for Width/Formant and cached by
default for Depth; the flags above regenerate all native outputs. Rebuild the
renderer after changing DSP code. `analyze_depth.py --skip-render` only analyzes
existing native WAVs. The report explains why cold impulses, unmatched bus
amplitudes, and raw output spectra alone can mislead this comparison.

`vocoder/build/render_probe --help` exposes a separate WAV-to-WAV renderer for
ad-hoc native probes, with explicit bands, width, depth, formant semitones,
frequency range, attack/release, input gain, wet mix, and settling/tail times.
It maps each ±1 WAV sample to ±5 DSP bus volts by default, then divides the
output by five. `--bus-volts-per-full-scale 1` uses the historical raw bus
convention. This is an explicit voltage convention; no peak normalization is
applied. It writes float WAV output with the vocoder's own protection active.

The [filterbank update](../../vocoder/fixtures/analysis/ableton_filterbank_update/README.md)
includes the fitted topology, validation, and an optional listening comparison.
After analyzing two revisions, make a before/after plot and level-matched
Formant audition without contacting Live:

```sh
python3 tools/live_probe/compare_revisions.py \
  --before /path/to/original-comparison \
  --after "$VOCODER_COMPARISON" --audition
```

`fit_filterbank.py --comparison BASELINE --output-dir DESTINATION` compares
candidate filter orders, polarity, and digital bandwidth compensation against
the stored complex response. `fit_depth.py --measurements BASELINE/analysis/depth-comparison.json --output DESTINATION/depth-law-fit.json`
fits static envelope-contrast candidates. Both tools read existing data and keep
the original audio untouched.

## Run and resume a batch

First test the Source → Capture chain with no effect between the bridge devices:

```sh
python3 -m tools.live_probe run tools/live_probe/examples/calibration.json
```

The calibration manifest has `"device": null`, so it never inspects or changes
effect parameters. This still records any processing physically present between
Source and Capture: remove effects for a direct calibration.

Copy and edit [examples/generic.json](examples/generic.json) for another effect.
It combines each value in `sweep` with each signal. Multiple sweep parameters
produce their Cartesian product. `parameters` holds fixed settings, and sweep
values override matching fixed settings. Use `parameters: {}` and `sweep: {}` to
measure the current sound without changing controls.

For selected combinations or one-control comparisons, use `cases` instead of a
nonempty `sweep`. Each case is a parameter override merged with the fixed
`parameters`; `{}` means the unchanged baseline. Each resulting setting is run
through every signal. Identical merged settings are recorded once:

```json
{
  "parameters": {"Filter Width": {"display": "100 %"}, "Envelope Depth": {"display": "100 %"}},
  "cases": [{}, {"Filter Width": {"display": "10 %"}}, {"Envelope Depth": {"display": "0 %"}}]
}
```

If a case omits a control changed by another case and no fixed value was given,
that control returns to its value at the start of the batch. Cases therefore
do not inherit the preceding case's settings.

```sh
# Prepare WAVs and the complete case plan without contacting Live.
python3 -m tools.live_probe run tools/live_probe/examples/generic.json --dry-run

# Run the prepared directory, or resume it after an interruption.
python3 -m tools.live_probe run --resume '/mnt/c/Users/tsarpf/Documents/Ableton/LiveProbe/runs/RUN_DIRECTORY'

# Or prepare and execute directly after editing the example's placeholders.
python3 -m tools.live_probe run my-effect.json
```

Each run gets a unique directory beneath `workspace/runs`; `--output-root` can
choose another Windows-accessible directory. Existing WAVs may live inside WSL:
the runner copies every input into the run directory before sending its Windows
path to Live. Relative WAV paths are resolved beside the manifest.

Completed cases are skipped on resume. The saved manifest and prepared input
hashes must still match. Failed recordings remain as `output.partial.wav` and
are retried on an explicit resume; completed recordings use `output.wav`.

The runner snapshots every parameter it will touch and restores those values on
completion, errors, and Ctrl+C when the bridge is reachable. It stops active
playback/recording during cleanup. The snapshot and any cleanup failures are
saved even when communication fails; inspect them before retrying after a Live
crash. Settings changed manually outside the batch are not tracked.

## Probe schema

Every signal has `kind` and an optional unique `name`. Times are in seconds,
frequencies in Hz. Signals are stereo PCM24 with identical channels unless
`right_gain_db` attenuates the right channel. A signal may last up to one hour;
a manifest expands to at most 10,000 cases. Unknown fields are rejected.

| `kind` | Fields and defaults |
| --- | --- |
| `impulse` | `duration: 2`, `offset_seconds: 0.1`, `level_dbfs: -18`, `right_gain_db: 0` |
| `sine` | `duration: 2`, `frequency: 440`, `level_dbfs: -18`, `right_gain_db: 0` |
| `stepped_sine` | `frequency: 440`, `levels_dbfs: [-48,-36,-24,-12,-6]`, `segment_seconds: 1`, `right_gain_db: 0`; continuous phase across levels |
| `noise` | `duration: 2`, `level_dbfs: -18`, `seed: 0`, `right_gain_db: 0`; deterministic uniform white noise |
| `sweep` | `duration: 2`, `start_frequency: 20`, `end_frequency: 20000`, `level_dbfs: -18`, `right_gain_db: 0`; logarithmic sine sweep |
| `silence` | `duration: 2` |
| `wav` | Required `path`; mono/stereo PCM or float WAV copied unchanged |

`level_dbfs` describes peak amplitude, including the ceiling of uniform noise;
noise RMS is approximately 4.77 dB below that ceiling. `stepped_sine` takes its
amplitudes from `levels_dbfs` and duration from the number of segments, so it
does not accept `duration` or `level_dbfs`.

Manifest timing fields are `settle_seconds` (default 0.5, silent settling after
setting parameters), `preroll_seconds` (default 0.1, recording before playback),
and `tail_seconds` (default 0.5, recording after playback finishes). Choose
settling and tails long enough for the effect's envelope/reverb state. The
effect instance stays loaded between cases; settling does not reset its state.

## Results and timing limits

`run.json` saves the expanded plan and input hashes. `session.json` saves bridge
identity, actual sample rate, device controls, and restoration values for that
invocation. Each case's `metadata.json` includes requested settings, parameter
readbacks, input/output format, timestamps, and whole-recording peak/RMS
statistics. RMS includes preroll and tail; silence is valid and flagged.
Non-finite output samples fail validation. `summary.json` records completion and
cleanup status.

Commands and playback completion are polled through files. The recording start
offset includes preroll, audio buffering, and scheduler delay; it is **not sample
accurate**. For impulse-response or phase measurements, align each recorded
impulse to the known input and use a direct Source → Capture calibration to
measure the route's latency. Preserve the unaligned captures as the original
data. Recording has no normalization or automatic loudness matching.

The direct chain has been tested with Windows Live at 48 kHz using impulses,
sine waves, and noise. Stereo outputs matched; the recorded sine peak was
−18.0000015 dBFS for a −18 dBFS probe. The tiny difference is consistent with
the input's PCM24 quantization. This verifies transport and level preservation;
effect matching requires its own measurements.

The 40-band Vocoder baseline was also tested at 48 kHz across all 22 distinct
width, depth, formant, and release settings using noise. Every recording
contained finite samples, and parameter restoration completed without errors.
The full example repeats those settings with three signals for 66 recordings.

The protocol allows one Python client per workspace and one command in flight
per endpoint. An unanswered command is retained after timeout so another client
cannot overwrite it. Open/reload the measurement set to receive a restart
response, then retry. A leftover `bridge/client.lock` from a killed Python
process must be removed only after checking no batch is still running. Run
`doctor` for endpoint errors, and inspect Live's Max console for audio or file
path failures. Do not rebuild the devices while measuring.

Run the Python transport, signal, and failure/recovery checks from the repo root:

```sh
python3 -m unittest discover -s tools/live_probe/tests -v
```
