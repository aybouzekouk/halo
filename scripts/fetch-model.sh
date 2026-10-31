#!/usr/bin/env bash
# Download the Voxtral weights (about 8.9 GB) into models/voxtral-realtime/.
#
# The link to the Hugging Face CDN is slow per connection here, so the file is
# fetched as N byte ranges in parallel. Every worker appends only the bytes it
# is still missing, so the whole thing is resumable: run the script again after
# a disconnect and it continues where it stopped. A range request that the
# server answers with 200 (instead of 206) would corrupt the file, so that case
# aborts the job instead of writing garbage.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$HERE/models/voxtral-realtime}"
PARTS="${PARTS:-8}"
REPO="https://huggingface.co/mistralai/Voxtral-Mini-4B-Realtime-2602/resolve/main"
URL="$REPO/model.safetensors"
OUT="$DEST/model.safetensors"
TMP="$DEST/.parts"
mkdir -p "$DEST" "$TMP"

for f in config.json generation_config.json processor_config.json tekken.json params.json; do
  [ -s "$DEST/$f" ] || curl -sSL --retry 5 -o "$DEST/$f" "$REPO/$f"
done

TOTAL=$(curl -sIL "$URL" | tr -d '\r' | awk 'tolower($1)=="content-length:"{v=$2} END{print v+0}')
if [ "$TOTAL" -lt 1000000 ]; then
  echo "could not read the remote size — check your connection" >&2
  exit 1
fi

if [ -s "$OUT" ] && [ "$(stat -c%s "$OUT")" -eq "$TOTAL" ]; then
  echo "already complete: $OUT"
  exit 0
fi

echo "weights: $((TOTAL/1000000)) MB in $PARTS parts"

worker() {
  local i="$1" start="$2" end="$3"
  local part="$TMP/part$i" want=$((end - start + 1)) have off code
  while true; do
    have=0
    [ -f "$part" ] && have=$(stat -c%s "$part")
    [ "$have" -ge "$want" ] && return 0
    off=$((start + have))
    code=$(curl -sS -L --retry 3 --retry-delay 2 --retry-all-errors \
                -r "${off}-${end}" -o "$TMP/chunk$i" -w '%{http_code}' "$URL" 2>/dev/null)
    if [ "$code" = "206" ] && [ -s "$TMP/chunk$i" ]; then
      cat "$TMP/chunk$i" >> "$part"
      rm -f "$TMP/chunk$i"
    elif [ "$code" = "200" ]; then
      echo "part$i: server ignored the byte range, aborting to avoid corruption" >&2
      rm -f "$TMP/chunk$i"
      return 3
    else
      sleep 3
    fi
  done
}

CHUNK=$(( (TOTAL + PARTS - 1) / PARTS ))
pids=()
for i in $(seq 0 $((PARTS-1))); do
  start=$(( i * CHUNK ))
  end=$(( start + CHUNK - 1 ))
  [ "$end" -ge "$TOTAL" ] && end=$((TOTAL-1))
  worker "$i" "$start" "$end" &
  pids+=($!)
done

status=0
for pid in "${pids[@]}"; do wait "$pid" || status=$?; done
if [ "$status" -ne 0 ]; then
  echo "a part failed; re-run to resume" >&2
  exit "$status"
fi

: > "$OUT.partial"
for i in $(seq 0 $((PARTS-1))); do cat "$TMP/part$i" >> "$OUT.partial"; done
SIZE=$(stat -c%s "$OUT.partial")
if [ "$SIZE" -ne "$TOTAL" ]; then
  echo "size mismatch: got $SIZE, expected $TOTAL — re-run to resume" >&2
  exit 2
fi
mv "$OUT.partial" "$OUT"
rm -rf "$TMP"
echo "complete: $OUT ($SIZE bytes)"
