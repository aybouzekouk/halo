#!/usr/bin/env bash
# Parallel ranged download of the Voxtral weights (HF supports Range), which is
# much faster than a single connection on slow links. Resumable.
set -u
URL="https://huggingface.co/mistralai/Voxtral-Mini-4B-Realtime-2602/resolve/main/model.safetensors"
OUT="${1:-/home/ayoub/halo/models/voxtral-realtime/model.safetensors}"
PARTS="${2:-8}"
TMP="$(dirname "$OUT")/.parts"
mkdir -p "$TMP"

TOTAL=$(curl -sIL "$URL" | tr -d '\r' | awk 'tolower($1)=="content-length:"{v=$2} END{print v}')
if [ -z "${TOTAL:-}" ] || [ "$TOTAL" -lt 1000000 ]; then
  echo "could not read the remote size (got: ${TOTAL:-none})" >&2
  exit 1
fi
echo "remote size: $TOTAL bytes, $PARTS parts"

CHUNK=$(( (TOTAL + PARTS - 1) / PARTS ))
pids=()
for i in $(seq 0 $((PARTS-1))); do
  start=$(( i * CHUNK ))
  end=$(( start + CHUNK - 1 ))
  [ "$end" -ge "$TOTAL" ] && end=$((TOTAL-1))
  want=$(( end - start + 1 ))
  part="$TMP/part$i"
  have=0
  [ -f "$part" ] && have=$(stat -c%s "$part")
  if [ "$have" -ge "$want" ]; then
    echo "part$i complete"
    continue
  fi
  (
    for attempt in $(seq 1 30); do
      curl -sSL --retry 3 --retry-delay 2 --retry-all-errors \
           -r "${start}-${end}" -o "$part" "$URL" && break
      sleep 2
    done
  ) &
  pids+=($!)
done

for pid in "${pids[@]:-}"; do wait "$pid" 2>/dev/null; done

cat "$TMP"/part* > "$OUT.partial"
SIZE=$(stat -c%s "$OUT.partial")
if [ "$SIZE" -ne "$TOTAL" ]; then
  echo "size mismatch: got $SIZE expected $TOTAL" >&2
  exit 2
fi
mv "$OUT.partial" "$OUT"
rm -rf "$TMP"
echo "saved $OUT ($SIZE bytes)"
