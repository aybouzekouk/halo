#!/usr/bin/env bash
# Install everything Halo needs to run entirely on this machine.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

if ! command -v uv >/dev/null 2>&1; then
  echo "uv is required (https://docs.astral.sh/uv/) — or install the packages from"
  echo "requirements.txt into .venv with pip, then run this script again."
  exit 1
fi

echo "== python environment =="
uv venv .venv --python 3.12
uv pip install --python .venv/bin/python -r requirements.txt

echo "== voxtral weights =="
if [ -s models/voxtral-realtime/model.safetensors ]; then
  echo "already present"
else
  ./scripts/fetch-model.sh
fi

echo "== piper voice =="
./scripts/fetch-voice.sh

echo
echo "Done. Start the voice model, then the app:"
echo "  ./scripts/start-asr.sh"
echo "  ./build/halo"
