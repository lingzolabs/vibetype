#!/usr/bin/env python3
"""Fake IBus client for Vibetype backend protocol debugging."""

import argparse
import json
import os
import shutil
import socket
import sys
import tempfile
import time
import uuid
import wave
from pathlib import Path


def default_socket_path() -> str:
    return os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "vibetype", "vibetype.sock")


def write_test_wav(path: Path, duration_ms: int) -> None:
    sample_rate = 16000
    frames = max(1, sample_rate * duration_ms // 1000)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(b"\x00\x00" * frames)


class JsonRpcClient:
    def __init__(self, socket_path: str):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(socket_path)
        self.file = self.sock.makefile("rwb", buffering=0)
        self.next_id = 1
        self.notifications = []

    def close(self) -> None:
        self.file.close()
        self.sock.close()

    def request(self, method: str, params: dict) -> dict:
        req_id = self.next_id
        self.next_id += 1
        message = {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method,
            "params": params,
        }
        self.file.write((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError("backend disconnected")
            msg = json.loads(line.decode("utf-8"))
            print("<-", json.dumps(msg, ensure_ascii=False))
            if msg.get("id") == req_id:
                if "error" in msg:
                    raise RuntimeError(f"RPC error: {msg['error']}")
                return msg["result"]
            self.notifications.append(msg)

    def read_until_notification(self, method: str, session_id: str) -> dict:
        while True:
            for index, msg in enumerate(self.notifications):
                if msg.get("method") == method and msg.get("params", {}).get("session_id") == session_id:
                    return self.notifications.pop(index)
            line = self.file.readline()
            if not line:
                raise RuntimeError("backend disconnected before notification")
            msg = json.loads(line.decode("utf-8"))
            print("<-", json.dumps(msg, ensure_ascii=False))
            if msg.get("method") == method and msg.get("params", {}).get("session_id") == session_id:
                return msg
            self.notifications.append(msg)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=default_socket_path())
    parser.add_argument("--segments", type=int, default=3)
    parser.add_argument("--duration-ms", type=int, default=200)
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--wav", default=None, help="Use this WAV as segment 0 instead of generating silence")
    parser.add_argument("--no-expect-fake", action="store_true", help="Do not assert fake ASR transcript text")
    parser.add_argument("--wait-model", action="store_true", help="Wait until vibetype.modelStatus reports ready")
    args = parser.parse_args()

    session_id = str(uuid.uuid4())
    if args.workdir:
        workdir = Path(args.workdir)
        workdir.mkdir(parents=True, exist_ok=True)
    else:
        base = Path(os.environ.get("XDG_RUNTIME_DIR", tempfile.gettempdir())) / "vibetype"
        base.mkdir(parents=True, exist_ok=True)
        workdir = Path(tempfile.mkdtemp(prefix=f"{session_id}-", dir=str(base)))

    print(f"session={session_id}")
    print(f"workdir={workdir}")

    client = JsonRpcClient(args.socket)
    try:
        client.request("vibetype.hello", {"client": "fake-ibus", "protocol_version": 1})
        if args.wait_model:
            for _ in range(600):
                status = client.request("vibetype.modelStatus", {})
                state = status.get("state")
                print("MODEL:", json.dumps(status, ensure_ascii=False))
                if state == "ready":
                    break
                if state == "error":
                    raise RuntimeError(f"model error: {status}")
                time.sleep(1)
            else:
                raise RuntimeError("model was not ready before timeout")
        client.request(
            "vibetype.startSession",
            {
                "session_id": session_id,
                "audio_format": {
                    "sample_rate": 16000,
                    "channels": 1,
                    "sample_format": "pcm_s16le",
                    "container": "wav",
                },
                "frontend": "fake-ibus",
            },
        )

        for index in range(args.segments):
            wav_path = workdir / f"segment-{index:03d}.wav"
            if args.wav and index == 0:
                shutil.copyfile(args.wav, wav_path)
            else:
                write_test_wav(wav_path, args.duration_ms)
            client.request(
                "vibetype.transcribeSegment",
                {
                    "session_id": session_id,
                    "segment_index": index,
                    "wav_path": str(wav_path),
                    "duration_ms": args.duration_ms,
                },
            )

        client.request(
            "vibetype.finishSession",
            {"session_id": session_id, "segment_count": args.segments},
        )
        final = client.read_until_notification("vibetype.finalResult", session_id)
        text = final["params"]["text"]
        if not args.no_expect_fake:
            expected = " ".join(f"fake transcript segment {i}" for i in range(args.segments))
            if text != expected:
                raise RuntimeError(f"unexpected final text: {text!r} != {expected!r}")
        print("COMMIT:", text)
        return 0
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
