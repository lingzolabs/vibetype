#!/usr/bin/env python3
"""Benchmark final-text polishing through a real Vibetype backend.

The backend is restarted for each case because fake ASR text is a startup-only
option. Use a Release build to keep the benchmark reasonably fast.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import uuid
import wave
from collections import Counter
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATASET = ROOT / "testdata/text-polishing/synthetic-stress.jsonl"
DEFAULT_BACKEND = ROOT / "build-release/bin/vibetype-backend"
DEFAULT_MODEL = Path.home() / ".config/vibetype/models/qwen3-0.6b-q4_k_m.gguf"


class RpcClient:
    def __init__(self, socket_path: Path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(30)
        self.sock.connect(str(socket_path))
        self.file = self.sock.makefile("rwb", buffering=0)
        self.next_id = 1
        self.notifications: list[dict[str, Any]] = []

    def close(self) -> None:
        self.file.close()
        self.sock.close()

    def request(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        request_id = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params}
        self.file.write((json.dumps(request, ensure_ascii=False) + "\n").encode())
        while True:
            message = self._read()
            if message.get("id") == request_id:
                if "error" in message:
                    raise RuntimeError(f"RPC error: {message['error']}")
                return message["result"]
            self.notifications.append(message)

    def notification(self, method: str, session_id: str) -> dict[str, Any]:
        while True:
            for index, message in enumerate(self.notifications):
                if (message.get("method") == method and
                        message.get("params", {}).get("session_id") == session_id):
                    return self.notifications.pop(index)
            message = self._read()
            if (message.get("method") == method and
                    message.get("params", {}).get("session_id") == session_id):
                return message
            self.notifications.append(message)

    def _read(self) -> dict[str, Any]:
        line = self.file.readline()
        if not line:
            raise RuntimeError("backend disconnected")
        return json.loads(line)


def load_dataset(path: Path) -> list[dict[str, Any]]:
    cases = []
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        case = json.loads(raw)
        for key in ("id", "category", "input", "acceptable_outputs"):
            if key not in case:
                raise ValueError(f"{path}:{line_number}: missing {key}")
        cases.append(case)
    return cases


def write_silence(path: Path) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(b"\x00\x00" * 1600)


def wait_for_socket(path: Path, process: subprocess.Popen[Any], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"backend exited with status {process.returncode}")
        if path.exists():
            return
        time.sleep(0.05)
    raise TimeoutError("backend socket was not created")


def wait_for_qwen(client: RpcClient, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = client.request("vibetype.hello", {"client": "polish-benchmark", "protocol_version": 1})
        qwen = status.get("qwen", {})
        if qwen.get("state") == "ready":
            return
        if qwen.get("state") == "error":
            raise RuntimeError(f"Qwen failed: {qwen.get('message', 'unknown error')}")
        time.sleep(0.1)
    raise TimeoutError("Qwen was not ready before timeout")


def transcribe_once(client: RpcClient, wav_path: Path) -> str:
    session_id = str(uuid.uuid4())
    client.request("vibetype.startSession", {
        "session_id": session_id,
        "audio_format": {
            "sample_rate": 16000,
            "channels": 1,
            "sample_format": "pcm_s16le",
            "container": "wav",
        },
        "frontend": "polish-benchmark",
    })
    client.request("vibetype.transcribeSegment", {
        "session_id": session_id,
        "segment_index": 0,
        "wav_path": str(wav_path),
        "duration_ms": 100,
    })
    client.request("vibetype.finishSession", {"session_id": session_id, "segment_count": 1})
    return client.notification("vibetype.finalResult", session_id)["params"]["text"]


def validate(case: dict[str, Any], output: str) -> tuple[bool, list[str]]:
    exact_match = output in case["acceptable_outputs"]
    hard_failures = []
    for value in case.get("must_contain", []):
        if value not in output:
            hard_failures.append(f"missing {value!r}")
    for value in case.get("must_not_contain", []):
        if value in output:
            hard_failures.append(f"unexpected {value!r}")
    maximum = case.get("max_output_chars")
    if maximum is not None and len(output) > maximum:
        hard_failures.append(f"output has {len(output)} chars, maximum is {maximum}")
    return exact_match, hard_failures


def stop_process(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def run_case(case: dict[str, Any], args: argparse.Namespace, config_home: Path,
             wav_path: Path, log_path: Path) -> tuple[str, float]:
    socket_path = config_home.parent / f"{case['id']}.sock"
    socket_path.unlink(missing_ok=True)
    command = [
        str(args.backend), "--fake-asr", "--fake-text", case["input"],
        "--socket", str(socket_path),
    ]
    env = os.environ.copy()
    env["XDG_CONFIG_HOME"] = str(config_home)
    env["XDG_RUNTIME_DIR"] = str(config_home.parent)
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, text=True)
        try:
            wait_for_socket(socket_path, process, args.startup_timeout)
            client = RpcClient(socket_path)
            try:
                wait_for_qwen(client, args.startup_timeout)
                output = transcribe_once(client, wav_path)
            finally:
                client.close()
        finally:
            stop_process(process)
            socket_path.unlink(missing_ok=True)
    return output, time.monotonic() - started


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, default=DEFAULT_DATASET)
    parser.add_argument("--backend", type=Path, default=DEFAULT_BACKEND)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--category", action="append", help="Run only these categories")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--startup-timeout", type=float, default=30)
    parser.add_argument("--n-ctx", type=int, default=512)
    parser.add_argument("--json-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.backend.is_file():
        print(f"backend not found: {args.backend}; build it in Release mode first", file=sys.stderr)
        return 2
    if not args.model.is_file():
        print(f"Qwen model not found: {args.model}", file=sys.stderr)
        return 2

    cases = load_dataset(args.dataset)
    if args.category:
        selected = set(args.category)
        cases = [case for case in cases if case["category"] in selected]
    if args.limit:
        cases = cases[:args.limit]
    if not cases:
        print("no benchmark cases selected", file=sys.stderr)
        return 2

    results = []
    with tempfile.TemporaryDirectory(prefix="vibetype-polish-benchmark-") as temp:
        temp_path = Path(temp)
        config_home = temp_path / "config"
        config_dir = config_home / "vibetype"
        config_dir.mkdir(parents=True)
        backend_config = {
            "qwen_enabled": True,
            "qwen_model": str(args.model.resolve()),
            "qwen_threads": max(1, os.cpu_count() or 4),
            "qwen_n_ctx": args.n_ctx,
        }
        (config_dir / "backend.json").write_text(
            json.dumps(backend_config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        text_config = json.loads((ROOT / "data/text-processing.json").read_text(encoding="utf-8"))
        text_config["enable_qwen_polish"] = True
        (config_dir / "text-processing.json").write_text(
            json.dumps(text_config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        runtime_workdir = temp_path / "vibetype"
        runtime_workdir.mkdir()
        wav_path = runtime_workdir / "silence.wav"
        write_silence(wav_path)

        for index, case in enumerate(cases, 1):
            log_path = temp_path / f"{case['id']}.log"
            try:
                output, elapsed = run_case(case, args, config_home, wav_path, log_path)
                exact_match, hard_failures = validate(case, output)
            except Exception as error:  # Keep the rest of the benchmark useful.
                output, elapsed, exact_match = "", 0.0, False
                hard_failures = [str(error)]
                if log_path.exists():
                    tail = log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-10:]
                    if tail:
                        hard_failures.append("backend log: " + " | ".join(tail))
            constraint_passed = not hard_failures
            passed = exact_match and constraint_passed
            results.append({
                "id": case["id"], "category": case["category"], "input": case["input"],
                "output": output, "passed": passed, "exact_match": exact_match,
                "constraint_passed": constraint_passed, "hard_failures": hard_failures,
                "elapsed_seconds": round(elapsed, 3),
            })
            marker = "PASS" if passed else ("SOFT" if constraint_passed else "FAIL")
            print(f"[{index:02d}/{len(cases):02d}] {marker} {case['id']} ({elapsed:.2f}s)")
            if not passed:
                print(f"  input:  {case['input']}")
                print(f"  output: {output}")
                if not exact_match:
                    print("  - not an acceptable exact output")
                for failure in hard_failures:
                    print(f"  - {failure}")

    passed_count = sum(result["passed"] for result in results)
    safe_count = sum(result["constraint_passed"] for result in results)
    category_total = Counter(result["category"] for result in results)
    category_passed = Counter(result["category"] for result in results if result["passed"])
    print(f"\nExact: {passed_count}/{len(results)} ({passed_count / len(results):.1%})")
    print(f"Constraints: {safe_count}/{len(results)} ({safe_count / len(results):.1%})")
    for category in sorted(category_total):
        print(f"  {category}: {category_passed[category]}/{category_total[category]} exact")

    report = {
        "dataset": str(args.dataset),
        "model": str(args.model),
        "n_ctx": args.n_ctx,
        "passed": passed_count,
        "constraint_passed": safe_count,
        "total": len(results),
        "results": results,
    }
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                    encoding="utf-8")
    return 0 if passed_count == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
