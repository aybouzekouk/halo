#!/usr/bin/env bash
# Download the Piper voice Halo speaks with (about 63 MB).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$HERE/models/piper"
mkdir -p "$DEST"
BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/amy/medium"
for f in en_US-amy-medium.onnx en_US-amy-medium.onnx.json; do
  if [ -s "$DEST/$f" ]; then
    echo "$f already present"
  else
    curl -sSL --retry 5 -C - -o "$DEST/$f" "$BASE/$f"
    echo "saved $DEST/$f"
  fi
done
