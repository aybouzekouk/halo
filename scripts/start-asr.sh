#!/usr/bin/env bash
# Start the local Voxtral realtime transcription sidecar.
# Keeps the model resident on the GPU and listens on ws://127.0.0.1:8765.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MODEL="${HALO_ASR_MODEL:-$HERE/models/voxtral-realtime}"
exec "$HERE/.venv/bin/python" "$HERE/python/asr_server.py" --model "$MODEL" "$@"
