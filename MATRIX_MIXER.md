# Matrix 12x16

A single disting NT plug-in algorithm implementing a 12-input, 16-destination
matrix mixer. It is intentionally one algorithm rather than twelve
stock mixer instances: this keeps preset slots free, makes the whole matrix
atomic, and permits a purpose-built overview UI.

## Display and controls

The disting NT display is **256 x 64 pixels with 16 grayscale levels**. The
left half is a compact 16-column (outputs) by 12-row (inputs) overview. Each
crosspoint is an intensity-coded pad and the selected pad is framed. A vertical
rule separates internal outputs 1-8 from expander outputs 1-8. The right
half shows the exact selected route and dB value above an always-visible meter
bridge for all 12 inputs and 16 outputs. An eleven-block horizontal gain strip
beside the dB value advances in 6 dB blocks: ten blocks reach unity and the
final block represents positive gain through +6 dB.

- Left encoder: select input row 1-12 or the output-trim row.
- Right encoder: select output column 1-16 or the input-trim column.
- Short-click either encoder: select only the hovered cell; `SEL` appears while
  it is active. A second short click returns to navigation.
- Hold and turn the left encoder: extend a one-column selection. Hold and turn
  the right encoder: extend a one-row selection. A selection can never become
  a rectangle. Subsequent encoder or centre-pot edits apply to the whole run.
- Either encoder in `EDIT`: change level in 0.1 dB steps, accelerating to
  0.5/2.0 dB only when turned quickly. Crossing unity snaps exactly to 0.0 dB.
- Centre pot: set the selected level with pickup. After selecting a different
  cell or editing by encoder, it cannot change the level until it crosses the
  selected cell's current value.
- Click the left pot: mute the hovered cell or selected run. Click it again to
  restore each cell's exact pre-mute dB value.
- Click the right pot: arm MIDI learn for the current cell or every cell in the
  active row/column selection; `LEARN` remains on screen until the next MIDI CC
  arrives. The selection is snapshotted when learn is armed. The learned
  channel and CC then control the whole snapshot and map 0-127 across `-inf` to
  +6 dB. Per-cell mappings and mute restore values are saved with the preset.
- Range: `-inf` to +6.0 dB. New routes default to `-inf`.

The `Internal inputs` parameter selects 1-12 active input rows. `Internal
outputs` and `Expander outputs` independently select 0-8 active columns in
their respective halves of the matrix. Navigation skips inactive rows and
columns. Disabled outputs are not written at all—even if their saved mode is
Replace—so setting `Expander outputs` to zero releases every NTX/aux
destination for other algorithms. Hidden crosspoint levels are retained and
return if channels are re-enabled.

The firmware will not restore a plug-in with the 192 crosspoints plus all of
the required native I/O parameters. The crosspoint values therefore live in
the algorithm and are saved as a 192-value array through the native plug-in
serialization API. Input/output trims, bus routing, output modes, and a small
`Route edit` helper remain normal host parameters. The helper can set any
crosspoint from a controller or script using `Route input`, `Route output`,
`Route level`, and the explicit `Apply route` trigger.

The 13th/bottom row contains the 16 output trims and the 17th/leftmost column
contains the 12 input trims. Their centre pixels are live level indicators. The
NT API does not expose host bus meters, so these are inexpensive peak followers
over the samples the matrix already reads and writes. NT audio-bus samples are
volt values, so input peaks are normalized against the hardware's 10 V maximum
magnitude before display; 5 V is therefore -6 dBFS instead of clipping the
meter. Input meters are pre-trim. Output meters show the complete destination
bus after this matrix writes, so they include earlier algorithms when an output
is in Add mode. Their 150 ms linear release is independent of block size.

## Bus layout and NTX-8CV

The first eight destinations use `First direct output`, defaulting to bus 13
(NT outputs 1-8). The second eight use the independent `First aux output`,
defaulting to bus 21 (aux buses 1-8). With an NTX-8CV configured for aux outputs
1-8, those destinations are written to the NTX automatically.

All 12 sources and all 16 destination/mode pairs are declared with the native
SDK audio I/O parameter types. Each destination independently supports Add or
Replace and defaults to Add. Replace writes silence when its crosspoint column
is muted; Add leaves earlier contributions on the destination intact.

Both eight-bus destination windows are independently configurable, and every
individual destination can also be changed. The DSP caches combined trim and
crosspoint gains plus a 12-bit active-source mask per output, so muted cells do
not consume audio-rate multiply work. Reducing any of the three channel-count
parameters also removes those channels from metering and DSP work.

## Build and install

```sh
make matrix
make test-matrix
make push-matrix PRESET=your-preset-name
```

The resulting plug-in is `matrix_mixer.o`; the algorithm appears as
`Matrix 12x16`.

The implementation targets the refreshed official distingNT API v13 at
`distingnt_api/` and requires firmware 1.15.0 or newer for that API version.
