#!/usr/bin/env python3
"""Round-trip check for the local speech sidecar.

Feeds a WAV file to python/asr_server.py over its websocket exactly the way the
Qt client does (100 ms frames of 16 kHz mono s16le) and prints the partials and
the final transcript.

    ./scripts/start-asr.sh &
    .venv/bin/python python/selftest_asr.py /tmp/question.wav
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import time

import numpy as np
import soundfile as sf
import websockets

TARGET_RATE = 16000


def load_pcm16k(path: str) -> bytes:
    audio, rate = sf.read(path, dtype="float32", always_2d=True)
    mono = audio.mean(axis=1)
    if rate != TARGET_RATE:
        duration = mono.size / rate
        count = int(round(duration * TARGET_RATE))
        x_old = np.linspace(0.0, duration, num=mono.size, endpoint=False)
        x_new = np.linspace(0.0, duration, num=count, endpoint=False)
        mono = np.interp(x_new, x_old, mono).astype(np.float32)
    pcm = np.clip(mono, -1.0, 1.0)
    return (pcm * 32767.0).astype("<i2").tobytes()


async def run(url: str, pcm: bytes, speed: float):
    frame = TARGET_RATE * 2 // 10  # 100 ms
    async with websockets.connect(url, max_size=8 * 1024 * 1024) as ws:
        started = time.time()
        while True:
            message = await ws.recv()
            event = json.loads(message)
            if event.get("type") == "status":
                print("status:", event.get("message"))
                continue
            if event.get("type") == "error":
                print("error:", event.get("message"))
                return 1
            if event.get("type") == "ready":
                print(f"ready in {time.time() - started:.1f}s")
                break
            if event.get("type") == "final":
                print("final:", event.get("text"))
                return 0

        await ws.send(json.dumps({"type": "start"}))
        for offset in range(0, len(pcm), frame):
            await ws.send(pcm[offset:offset + frame])
            await asyncio.sleep((frame / 2) / TARGET_RATE / max(speed, 0.1))

        # let the last partials arrive before ending the utterance
        await asyncio.sleep(0.4)
        await ws.send(json.dumps({"type": "stop"}))

        while True:
            event = json.loads(await ws.recv())
            if event.get("type") == "partial":
                print("partial:", event.get("text"))
            elif event.get("type") == "final":
                print("final:", event.get("text"))
                return 0
            elif event.get("type") == "error":
                print("error:", event.get("message"))
                return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav")
    parser.add_argument("--url", default="ws://127.0.0.1:8765")
    parser.add_argument("--speed", type=float, default=1.0,
                        help="1.0 streams in real time, 2.0 twice as fast")
    args = parser.parse_args()
    pcm = load_pcm16k(args.wav)
    print(f"{len(pcm) / (TARGET_RATE * 2):.1f}s of audio")
    try:
        return asyncio.run(run(args.url, pcm, args.speed))
    except KeyboardInterrupt:
        return 1


if __name__ == "__main__":
    sys.exit(main())
