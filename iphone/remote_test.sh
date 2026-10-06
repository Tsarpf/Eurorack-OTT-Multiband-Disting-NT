#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PHONE=${PHONE:-iphonex}
SSH_USER=${SSH_USER:-root}
SSH_TARGET=${SSH_USER}@${PHONE}
MOBILE_SSH_TARGET=${MOBILE_SSH_USER:-mobile}@${PHONE}
REMOTE_DATA=${REMOTE_DATA:-/var/jb/var/mobile}
REMOTE_BIN=${REMOTE_BIN:-/var/jb/usr/bin}
TROLL_HELPER=${TROLL_HELPER:-}
LOCAL_PORT=${LOCAL_PORT:-47831}
EFFECT_LOCAL_PORT=${EFFECT_LOCAL_PORT:-47830}

echo "Building the AU effect, AU host, headless host, probes, and SSH launcher..."
make -C "$ROOT_DIR/iphone" release host-release host-daemon au-direct-probe launchapp

if [ -z "$TROLL_HELPER" ]; then
  TROLL_HELPER=$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$SSH_TARGET" \
    'for p in /var/containers/Bundle/Application/*/TrollStore.app/trollstorehelper; do [ -x "$p" ] && { echo "$p"; break; }; done')
fi
if [ -z "$TROLL_HELPER" ]; then
  echo "TrollStore helper was not found on $PHONE" >&2
  exit 2
fi

EFFECT_IPA_NAME=VocoderBridge-0.3.1-build10.ipa
HOST_IPA_NAME=VocoderHost-0.1.1-build4.ipa
REMOTE_EFFECT_IPA=$REMOTE_DATA/$EFFECT_IPA_NAME
REMOTE_HOST_IPA=$REMOTE_DATA/$HOST_IPA_NAME
scp -q "$ROOT_DIR/iphone/build/$EFFECT_IPA_NAME" "$SSH_TARGET:$REMOTE_EFFECT_IPA"
scp -q "$ROOT_DIR/iphone/build/$HOST_IPA_NAME" "$SSH_TARGET:$REMOTE_HOST_IPA"
scp -q "$ROOT_DIR/iphone/build/au-direct-probe" "$SSH_TARGET:$REMOTE_BIN/au-direct-probe"
scp -q "$ROOT_DIR/iphone/build/VocoderHostDaemon" "$SSH_TARGET:$REMOTE_BIN/VocoderHostDaemon"
scp -q "$ROOT_DIR/iphone/build/ai-launchapp" "$SSH_TARGET:$REMOTE_BIN/ai-launchapp"
ssh -o BatchMode=yes -o ConnectTimeout=8 "$SSH_TARGET" \
  "chmod 755 '$REMOTE_BIN/au-direct-probe' '$REMOTE_BIN/ai-launchapp'; '$TROLL_HELPER' install '$REMOTE_EFFECT_IPA'; '$TROLL_HELPER' install '$REMOTE_HOST_IPA'"

echo "Running the AU render test directly on the phone..."
ssh -o BatchMode=yes -o ConnectTimeout=8 "$SSH_TARGET" \
  "$REMOTE_BIN/au-direct-probe --wav $REMOTE_DATA/VocoderAU-test.wav"
scp -q "$SSH_TARGET:$REMOTE_DATA/VocoderAU-test.wav" \
  "$ROOT_DIR/iphone/build/VocoderAU-test.wav"
python3 "$ROOT_DIR/iphone/validate_wav.py" \
  "$ROOT_DIR/iphone/build/VocoderAU-test.wav"

try_effect_app() (
  echo "Requesting a foreground launch of the effect app..."
  set +e
  EFFECT_LAUNCH_OUTPUT=$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$SSH_TARGET" \
    "$REMOTE_BIN/ai-launchapp com.tsarpf.vocoderbridge 2>&1")
  EFFECT_LAUNCH_RC=$?
  set -e
  printf '%s\n' "$EFFECT_LAUNCH_OUTPUT"
  if [ "$EFFECT_LAUNCH_RC" -ne 0 ]; then
    echo "Effect app launch was rejected (the phone may be locked)."
    exit 1
  fi

  EFFECT_TUNNEL_PID=
  effect_cleanup() {
    if [ -n "$EFFECT_TUNNEL_PID" ]; then
      kill "$EFFECT_TUNNEL_PID" 2>/dev/null || true
    fi
  }
  trap effect_cleanup EXIT INT TERM
  ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes -o ConnectTimeout=8 \
    -L "$EFFECT_LOCAL_PORT:127.0.0.1:47821" "$SSH_TARGET" &
  EFFECT_TUNNEL_PID=$!
  sleep 1
  for _ in $(seq 1 20); do
    if curl -fsS --max-time 2 "http://127.0.0.1:$EFFECT_LOCAL_PORT/state"; then
      echo
      curl -fsS --max-time 8 "http://127.0.0.1:$EFFECT_LOCAL_PORT/au-test"
      echo
      effect_cleanup
      exit 0
    fi
    sleep 1
  done
  effect_cleanup
  echo "The effect app launch returned success, but its control endpoint did not respond."
  exit 1
)

try_effect_app || true

run_locked_daemon_test() (
  echo "Starting the headless host daemon."
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$SSH_TARGET" \
    "killall -9 VocoderHostDaemon 2>/dev/null || true; chmod 755 '$REMOTE_BIN/VocoderHostDaemon'"
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$MOBILE_SSH_TARGET" \
    "nohup '$REMOTE_BIN/VocoderHostDaemon' >'$REMOTE_DATA/vocoder-daemon.log' 2>&1 </dev/null &"
  TUNNEL_PID=
  cleanup() {
    if [ -n "$TUNNEL_PID" ]; then
      kill "$TUNNEL_PID" 2>/dev/null || true
    fi
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$SSH_TARGET" \
      "killall -9 VocoderHostDaemon 2>/dev/null || true" >/dev/null 2>&1 || true
  }
  trap cleanup EXIT INT TERM
  ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes -o ConnectTimeout=8 \
    -L "$LOCAL_PORT:127.0.0.1:47822" "$SSH_TARGET" &
  TUNNEL_PID=$!
  sleep 1
  for _ in $(seq 1 20); do
    set +e
    STATE=$(curl -fsS --max-time 2 "http://127.0.0.1:$LOCAL_PORT/state")
    STATE_RC=$?
    set -e
    if [ "$STATE_RC" -ne 0 ]; then
      sleep 1
      continue
    fi
    printf '%s\n' "$STATE"
    if ! printf '%s' "$STATE" | python3 -c \
        'import json,sys; d=json.load(sys.stdin); raise SystemExit(0 if d.get("ready") else 1)'; then
      echo "The headless host is reachable but not ready." >&2
      exit 3
    fi
    set +e
    AU_TEST=$(curl -fsS --max-time 20 "http://127.0.0.1:$LOCAL_PORT/au-test")
    AU_TEST_RC=$?
    set -e
    printf '%s\n' "$AU_TEST"
    if [ "$AU_TEST_RC" -ne 0 ] || ! printf '%s' "$AU_TEST" | python3 -c \
        'import json,sys; d=json.load(sys.stdin); raise SystemExit(0 if d.get("ok") else 1)'; then
      echo "The headless host render test failed." >&2
      exit 3
    fi
    set +e
    curl -fsS --max-time 20 "http://127.0.0.1:$LOCAL_PORT/render.wav" \
      > "$ROOT_DIR/iphone/build/VocoderHost-test.wav"
    WAV_RC=$?
    set -e
    if [ "$WAV_RC" -ne 0 ] || ! python3 "$ROOT_DIR/iphone/validate_wav.py" \
        "$ROOT_DIR/iphone/build/VocoderHost-test.wav"; then
      echo "The headless host did not return a valid render file." >&2
      exit 3
    fi
    echo "saved $ROOT_DIR/iphone/build/VocoderHost-test.wav"
    exit 0
  done
  echo "The headless host daemon did not respond." >&2
  exit 3
)

try_host_app() (
  echo "Requesting a foreground launch of the AU host..."
  set +e
  LAUNCH_OUTPUT=$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$SSH_TARGET" \
    "$REMOTE_BIN/ai-launchapp com.tsarpf.vocoder.host 2>&1")
  LAUNCH_RC=$?
  set -e
  printf '%s\n' "$LAUNCH_OUTPUT"
  if [ "$LAUNCH_RC" -ne 0 ]; then
    echo "The foreground launch was rejected (the phone may be locked)."
    exit 1
  fi

  TUNNEL_PID=
  cleanup() {
    if [ -n "$TUNNEL_PID" ]; then
      kill "$TUNNEL_PID" 2>/dev/null || true
    fi
  }
  trap cleanup EXIT INT TERM
  ssh -N -o BatchMode=yes -o ExitOnForwardFailure=yes -o ConnectTimeout=8 \
    -L "$LOCAL_PORT:127.0.0.1:47822" "$SSH_TARGET" &
  TUNNEL_PID=$!
  sleep 1

  for _ in $(seq 1 20); do
    set +e
    STATE=$(curl -fsS --max-time 2 "http://127.0.0.1:$LOCAL_PORT/state")
    STATE_RC=$?
    set -e
    if [ "$STATE_RC" -ne 0 ]; then
      sleep 1
      continue
    fi
    printf '%s\n' "$STATE"
    if ! printf '%s' "$STATE" | python3 -c \
        'import json,sys; d=json.load(sys.stdin); raise SystemExit(0 if d.get("ready") else 1)'; then
      echo "The AU host is reachable but not ready; its registry error is above."
      exit 1
    fi

    set +e
    AU_TEST=$(curl -fsS --max-time 20 "http://127.0.0.1:$LOCAL_PORT/au-test")
    AU_TEST_RC=$?
    set -e
    printf '%s\n' "$AU_TEST"
    if [ "$AU_TEST_RC" -ne 0 ] || ! printf '%s' "$AU_TEST" | python3 -c \
        'import json,sys; d=json.load(sys.stdin); raise SystemExit(0 if d.get("ok") else 1)'; then
      echo "The AU host could not complete its render test."
      exit 1
    fi

    set +e
    curl -fsS --max-time 20 "http://127.0.0.1:$LOCAL_PORT/render.wav" \
      > "$ROOT_DIR/iphone/build/VocoderHost-test.wav"
    WAV_RC=$?
    set -e
    if [ "$WAV_RC" -ne 0 ] || ! python3 "$ROOT_DIR/iphone/validate_wav.py" \
        "$ROOT_DIR/iphone/build/VocoderHost-test.wav"; then
      echo "The AU host did not return a valid render file."
      exit 1
    fi
    echo "saved $ROOT_DIR/iphone/build/VocoderHost-test.wav"
    exit 0
  done

  echo "The AU host launch returned success, but its control endpoint did not respond."
  exit 1
)

if try_host_app; then
  exit 0
fi

echo "Using the headless host daemon for the automated render test."
run_locked_daemon_test
