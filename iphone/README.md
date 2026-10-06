# Vocoder Bridge remote test

The repeatable phone workflow is:

```sh
./iphone/remote_test.sh
```

It builds the AUv3 effect and the separate `VocoderHost` AUv3 host, creates the
versioned `build/VocoderBridge-0.3.1-build10.ipa` and
`build/VocoderHost-0.1.1-build4.ipa`, installs both through TrollStore over the
configured `root@iphonex` SSH key, deploys the jailbreak-side probes, and runs
the AU render test on the phone. If SpringBoard permits a foreground launch,
the script also checks the host's `/state`, `/au-test`, and `/render.wav`
endpoints through a loopback HTTP tunnel. If the foreground host is reachable but not ready, crashes, or returns an invalid render, the script records that result and automatically falls back to the same headless daemon path.

When the phone is locked, the script starts a temporary mobile-user
`VocoderHostDaemon` instead. It uses the same AU implementation for a
deterministic host-side render and retrieves the resulting WAV before stopping
the daemon. The installed `VocoderHost` app remains the registry-based path to
exercise in Loopy Pro once the device is unlocked.

Every retrieved WAV is checked locally by `validate_wav.py` for the expected
48 kHz stereo float format, finite samples, and non-silent output before the
script succeeds.

The direct probe is independent of the UI and works while the device is
locked. A foreground app scene is still subject to SpringBoard’s lock-screen
policy; a locked-device launch therefore reports that condition and leaves the
successful direct AU result intact.
