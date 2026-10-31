#!/usr/bin/env bash
# End-to-end check of the local speech model: wait for the weights, start the
# sidecar, feed it a spoken question, and print what it heard.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODEL="$HERE/models/voxtral-realtime/model.safetensors"
WAV="${1:-/tmp/opencode/q16.wav}"
PORT="${2:-8769}"
LOG=/tmp/opencode/asr-verify.log

echo "waiting for $MODEL ..."
for _ in $(seq 1 1440); do
  if [ -s "$MODEL" ] && [ "$(stat -c%s "$MODEL")" -gt 8000000000 ]; then break; fi
  sleep 10
done
[ -s "$MODEL" ] || { echo "FAIL: weights missing"; exit 1; }
echo "weights present: $(stat -c%s "$MODEL") bytes"

"$HERE/scripts/start-asr.sh" --port "$PORT" > "$LOG" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT

for _ in $(seq 1 120); do
  grep -q "serving" "$LOG" && break
  grep -qi "failed to load" "$LOG" && { echo "FAIL: model did not load"; tail -20 "$LOG"; exit 1; }
  sleep 5
done
grep -q "serving" "$LOG" || { echo "FAIL: timeout"; tail -20 "$LOG"; exit 1; }
echo "sidecar ready"
tail -5 "$LOG"

"$HERE/.venv/bin/python" "$HERE/python/selftest_asr.py" "$WAV" --url "ws://127.0.0.1:$PORT" --speed 1.5
echo "PASS"
