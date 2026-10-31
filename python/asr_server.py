#!/usr/bin/env python3
"""Local speech-to-text sidecar for Halo.

Loads mistralai/Voxtral-Mini-4B-Realtime-2602 with Transformers, keeps the
weights on the GPU, and serves a tiny websocket protocol on 127.0.0.1:8765:

    client -> server
        {"type": "hello"}             optional, ignored
        {"type": "start"}             begin a new utterance
        <binary frame>                raw PCM, 16 kHz mono, signed 16-bit LE
        {"type": "stop"}              finish and transcribe everything buffered
        {"type": "reset"}             throw the buffer away

    server -> client
        {"type": "ready"}             the model is loaded and warm
        {"type": "status", "message"}
        {"type": "partial", "text"}   best transcript so far (replace, do not append)
        {"type": "final", "text"}
        {"type": "error", "message"}

While audio is streaming the server re-transcribes the current utterance every
few hundred milliseconds, which gives live partials without needing a bespoke
streaming decoder. On "stop" it runs one final pass over the whole utterance.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

MODEL_ID = "mistralai/Voxtral-Mini-4B-Realtime-2602"

engine = None            # set once the model is loaded
args_stub_text = None
engine_error = None
ready_event = None       # asyncio.Event, set when the weights are loaded
main_loop = None         # the asyncio loop, for the loader thread
executor = ThreadPoolExecutor(max_workers=1)


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


class Engine:
    def __init__(self, model_path: str, device: str, dtype: str):
        import torch
        from transformers import AutoProcessor, VoxtralRealtimeForConditionalGeneration

        self.torch = torch
        self.device = device
        self.dtype = getattr(torch, dtype)

        log(f"loading processor from {model_path}")
        self.processor = AutoProcessor.from_pretrained(model_path)
        log(f"loading weights from {model_path} ({dtype})")
        # Load on the CPU and move afterwards: this keeps `accelerate` out of
        # the dependency list and the 9 GB of weights fit in system RAM.
        self.model = VoxtralRealtimeForConditionalGeneration.from_pretrained(
            model_path,
            dtype=self.dtype,
        )
        self.model.to(device)
        self.model.eval()
        self.rate = int(self.processor.feature_extractor.sampling_rate)
        log(f"model ready on {device} at {self.rate} Hz")

    def _prepare(self, audio: np.ndarray):
        try:
            inputs = self.processor(audio, sampling_rate=self.rate, return_tensors="pt")
        except TypeError:
            # Older/newer processor signatures do not accept sampling_rate.
            inputs = self.processor(audio, return_tensors="pt")
        moved = {}
        for key, value in inputs.items():
            if not hasattr(value, "to"):
                moved[key] = value
                continue
            if value.is_floating_point():
                moved[key] = value.to(self.device, dtype=self.dtype)
            else:
                moved[key] = value.to(self.device)
        return moved

    def transcribe(self, audio: np.ndarray) -> str:
        audio = np.ascontiguousarray(audio, dtype=np.float32)
        if audio.size < int(0.2 * self.rate):
            return ""
        with self.torch.inference_mode():
            inputs = self._prepare(audio)
            output = self.model.generate(**inputs)
        text = self.processor.batch_decode(output, skip_special_tokens=True)[0]
        return " ".join(text.split())

    def warmup(self):
        log("warming up")
        self.transcribe(np.zeros(self.rate, dtype=np.float32))
        log("warm")


class StubEngine:
    """No weights, canned text: lets you test the socket and the UI wiring."""

    rate = 16000

    def __init__(self, text: str):
        self.text = text

    def transcribe(self, audio: np.ndarray) -> str:
        # Reveal the canned answer proportionally to how much audio arrived.
        seconds = audio.size / self.rate
        share = min(1.0, seconds / 2.6)
        words = self.text.split()
        visible = max(1, int(round(len(words) * share)))
        return " ".join(words[:visible])

    def warmup(self):
        pass


def load_engine(model_path: str, device: str, dtype: str):
    global engine, engine_error
    try:
        if args_stub_text is not None:
            eng = StubEngine(args_stub_text)
        else:
            eng = Engine(model_path, device, dtype)
        eng.warmup()
        engine = eng
        log("serving")
    except Exception as exc:  # noqa: BLE001 - surfaced to the client
        engine_error = f"{type(exc).__name__}: {exc}"
        log("model failed to load:", engine_error)
    finally:
        if ready_event is not None and main_loop is not None:
            main_loop.call_soon_threadsafe(ready_event.set)


class Session:
    def __init__(self, websocket):
        self.ws = websocket
        self.audio = np.zeros(0, dtype=np.float32)
        self.dirty = False
        self.max_seconds = 30.0
        self.last_text = ""
        self.closed = False
        self.partial_task = None
        self.started_at = time.time()

    async def send(self, payload: dict):
        if self.closed:
            return
        try:
            await self.ws.send(json.dumps(payload))
        except Exception:
            self.closed = True

    def feed(self, data: bytes):
        pcm = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768.0
        self.audio = np.concatenate([self.audio, pcm])
        limit = int(self.max_seconds * (engine.rate if engine else 16000))
        if self.audio.size > limit:
            self.audio = self.audio[-limit:]
        self.dirty = True

    async def partial_loop(self):
        loop = asyncio.get_running_loop()
        while not self.closed:
            await asyncio.sleep(0.35)
            if engine is None or not self.dirty or self.closed:
                continue
            if self.audio.size < int(0.45 * engine.rate):
                continue
            self.dirty = False
            snapshot = self.audio.copy()
            try:
                text = await loop.run_in_executor(executor, engine.transcribe, snapshot)
            except Exception as exc:  # noqa: BLE001
                await self.send({"type": "error", "message": f"{type(exc).__name__}: {exc}"})
                continue
            if text and text != self.last_text and not self.closed:
                self.last_text = text
                await self.send({"type": "partial", "text": text})

    async def announce_ready(self):
        if engine is None and engine_error is None:
            await self.send({"type": "status", "message": "loading the local voice model"})
            await ready_event.wait()
        if engine_error:
            await self.send({"type": "error", "message": engine_error})
        else:
            await self.send({"type": "ready"})

    async def run(self):
        loop = asyncio.get_running_loop()
        self.partial_task = asyncio.create_task(self.partial_loop())
        self.ready_task = asyncio.create_task(self.announce_ready())

        try:
            async for message in self.ws:
                if isinstance(message, (bytes, bytearray)):
                    if engine is None:
                        # Hold a little audio while the weights load.
                        if self.audio.size < int(4 * 16000):
                            self.feed(bytes(message))
                        continue
                    self.feed(bytes(message))
                    continue

                try:
                    event = json.loads(message)
                except json.JSONDecodeError:
                    continue
                kind = event.get("type")

                if kind == "start":
                    self.audio = np.zeros(0, dtype=np.float32)
                    self.last_text = ""
                    self.dirty = False
                    self.started_at = time.time()
                elif kind == "reset":
                    self.audio = np.zeros(0, dtype=np.float32)
                    self.last_text = ""
                    self.dirty = False
                elif kind == "stop":
                    if engine is None:
                        await self.send({"type": "error",
                                         "message": "the local voice model is still loading"})
                        await self.send({"type": "final", "text": ""})
                        continue
                    self.dirty = False
                    snapshot = self.audio.copy()
                    text = ""
                    if snapshot.size >= int(0.25 * engine.rate):
                        text = await loop.run_in_executor(executor, engine.transcribe, snapshot)
                    await self.send({"type": "final", "text": text})
                    self.audio = np.zeros(0, dtype=np.float32)
                    self.last_text = ""
                    self.dirty = False
                elif kind == "ping":
                    await self.send({"type": "pong", "t": time.time()})
                elif kind == "hello":
                    if engine is not None:
                        await self.send({"type": "ready"})
                    elif engine_error:
                        await self.send({"type": "error", "message": engine_error})
        except Exception as exc:  # noqa: BLE001 - connection level
            log("session ended:", type(exc).__name__, exc)
        finally:
            self.closed = True
            if self.partial_task:
                self.partial_task.cancel()
            if getattr(self, "ready_task", None):
                self.ready_task.cancel()


async def main_async(args):
    global ready_event, args_stub_text, main_loop
    ready_event = asyncio.Event()
    main_loop = asyncio.get_running_loop()
    args_stub_text = args.stub

    import websockets

    model_path = args.model or MODEL_ID
    loop = asyncio.get_running_loop()
    loop.run_in_executor(None, load_engine, model_path, args.device, args.dtype)

    async def handler(websocket):
        await Session(websocket).run()

    async with websockets.serve(handler, args.host, args.port, max_size=8 * 1024 * 1024):
        log(f"listening on ws://{args.host}:{args.port}  (model: {model_path})")
        await asyncio.Future()


def main():
    parser = argparse.ArgumentParser(description="Halo local Voxtral realtime ASR sidecar")
    parser.add_argument("--model", default=os.environ.get("HALO_ASR_MODEL", ""),
                        help=f"local model directory or hub id (default: {MODEL_ID})")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--dtype", default="bfloat16",
                        help="bfloat16, float16 or float32")
    parser.add_argument("--stub", nargs="?", const="Can you explain what a low opacity glow layer does",
                        default=None,
                        help="skip the model and answer with this fixed text (protocol/UI testing)")
    args = parser.parse_args()

    try:
        asyncio.run(main_async(args))
    except KeyboardInterrupt:
        log("stopped")
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
