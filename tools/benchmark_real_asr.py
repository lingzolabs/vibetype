#!/usr/bin/env python3
"""Compare real SenseVoice transcripts with and without Qwen polishing."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unicodedata
from pathlib import Path
from typing import Any

from benchmark_text_polishing import RpcClient, stop_process, transcribe_once, wait_for_socket

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "testdata/fleurs/manifest.jsonl"
DEFAULT_BACKEND = ROOT / "build-release/bin/vibetype-backend"
DEFAULT_ASR_MODEL = Path.home() / ".config/vibetype/models/sensevoice-small-q8.gguf"
DEFAULT_QWEN_MODEL = Path.home() / ".config/vibetype/models/qwen3-0.6b-q4_k_m.gguf"


def normalized_characters(text: str) -> list[str]:
    text = unicodedata.normalize("NFKC", text).casefold()
    return [char for char in text if unicodedata.category(char)[0] in {"L", "N"}]


def edit_distance(left: list[str], right: list[str]) -> int:
    if len(left) < len(right):
        left, right = right, left
    previous = list(range(len(right) + 1))
    for i, left_item in enumerate(left, 1):
        current = [i]
        for j, right_item in enumerate(right, 1):
            current.append(min(
                current[-1] + 1,
                previous[j] + 1,
                previous[j - 1] + (left_item != right_item),
            ))
        previous = current
    return previous[-1]


def score(reference: str, hypothesis: str) -> tuple[int, int]:
    reference_chars = normalized_characters(reference)
    hypothesis_chars = normalized_characters(hypothesis)
    return edit_distance(reference_chars, hypothesis_chars), len(reference_chars)


def load_manifest(path: Path) -> list[dict[str, Any]]:
    samples = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        sample = json.loads(line)
        for key in ("language", "audio", "raw_transcription"):
            if key not in sample:
                raise ValueError(f"{path}:{line_number}: missing {key}")
        samples.append(sample)
    return samples


def wait_for_models(client: RpcClient, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = client.request("vibetype.hello", {
            "client": "real-asr-benchmark", "protocol_version": 1,
        })
        model = status.get("model", {})
        qwen = status.get("qwen", {})
        errors = [item for item in (model, qwen) if item.get("state") == "error"]
        if errors:
            raise RuntimeError("model load failed: " + "; ".join(
                item.get("message", "unknown error") for item in errors))
        if model.get("state") == "ready" and qwen.get("state") == "ready":
            return
        time.sleep(0.1)
    raise TimeoutError("models were not ready before timeout")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--backend", type=Path, default=DEFAULT_BACKEND)
    parser.add_argument("--asr-model", type=Path, default=DEFAULT_ASR_MODEL)
    parser.add_argument("--qwen-model", type=Path, default=DEFAULT_QWEN_MODEL)
    parser.add_argument("--language", action="append")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--threads", type=int, default=max(1, os.cpu_count() or 4))
    parser.add_argument("--startup-timeout", type=float, default=60)
    parser.add_argument("--json-output", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    for path, label in ((args.backend, "backend"), (args.asr_model, "ASR model"),
                        (args.qwen_model, "Qwen model")):
        if not path.is_file():
            print(f"{label} not found: {path}", file=sys.stderr)
            return 2

    samples = load_manifest(args.manifest)
    if args.language:
        selected = set(args.language)
        samples = [sample for sample in samples if sample["language"] in selected]
    if args.limit:
        samples = samples[:args.limit]
    if not samples:
        print("no samples selected", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="vibetype-real-asr-") as temp:
        temp_path = Path(temp)
        config_home = temp_path / "config"
        config_dir = config_home / "vibetype"
        runtime_dir = temp_path / "runtime"
        runtime_workdir = runtime_dir / "vibetype"
        config_dir.mkdir(parents=True)
        runtime_workdir.mkdir(parents=True)
        socket_path = runtime_dir / "backend.sock"
        log_path = temp_path / "backend.log"

        backend_config = {
            "model": str(args.asr_model.resolve()),
            "threads": args.threads,
            "qwen_enabled": True,
            "qwen_model": str(args.qwen_model.resolve()),
            "qwen_threads": args.threads,
            "qwen_n_ctx": 512,
        }
        (config_dir / "backend.json").write_text(
            json.dumps(backend_config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        text_config_path = config_dir / "text-processing.json"
        text_config = json.loads((ROOT / "data/text-processing.json").read_text(encoding="utf-8"))
        text_config["enable_qwen_polish"] = False
        text_config_path.write_text(
            json.dumps(text_config, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

        env = os.environ.copy()
        env["XDG_CONFIG_HOME"] = str(config_home)
        env["XDG_RUNTIME_DIR"] = str(runtime_dir)
        command = [str(args.backend), "--socket", str(socket_path)]
        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, text=True)
            try:
                wait_for_socket(socket_path, process, args.startup_timeout)
                client = RpcClient(socket_path)
                try:
                    wait_for_models(client, args.startup_timeout)
                    prepared = []
                    for index, sample in enumerate(samples):
                        source = ROOT / sample["audio"]
                        destination = runtime_workdir / f"sample-{index:03d}.wav"
                        shutil.copyfile(source, destination)
                        prepared.append(destination)

                    baseline = []
                    for index, (sample, audio) in enumerate(zip(samples, prepared), 1):
                        started = time.monotonic()
                        text = transcribe_once(client, audio)
                        baseline.append((text, time.monotonic() - started))
                        print(f"[{index:02d}/{len(samples):02d}] ASR {sample['language']} {text}")

                    text_config["enable_qwen_polish"] = True
                    text_config_path.write_text(
                        json.dumps(text_config, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")
                    client.request("vibetype.reloadConfig", {})

                    polished = []
                    for index, (sample, audio) in enumerate(zip(samples, prepared), 1):
                        started = time.monotonic()
                        text = transcribe_once(client, audio)
                        polished.append((text, time.monotonic() - started))
                        print(f"[{index:02d}/{len(samples):02d}] LLM {sample['language']} {text}")
                finally:
                    client.close()
            except Exception:
                log.flush()
                print("backend log tail:", file=sys.stderr)
                for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines()[-30:]:
                    print(line, file=sys.stderr)
                raise
            finally:
                stop_process(process)

    results = []
    base_errors = polished_errors = reference_chars = 0
    improved = unchanged_score = regressed = 0
    for sample, (base_text, base_seconds), (polished_text, polished_seconds) in zip(
            samples, baseline, polished):
        base_error, length = score(sample["raw_transcription"], base_text)
        polished_error, _ = score(sample["raw_transcription"], polished_text)
        base_errors += base_error
        polished_errors += polished_error
        reference_chars += length
        if polished_error < base_error:
            outcome = "improved"
            improved += 1
        elif polished_error > base_error:
            outcome = "regressed"
            regressed += 1
        else:
            outcome = "same-score"
            unchanged_score += 1
        results.append({
            "language": sample["language"], "audio": sample["audio"],
            "reference": sample["raw_transcription"], "baseline": base_text,
            "polished": polished_text, "baseline_edits": base_error,
            "polished_edits": polished_error, "reference_chars": length,
            "outcome": outcome, "baseline_seconds": round(base_seconds, 3),
            "polished_seconds": round(polished_seconds, 3),
        })

    base_cer = base_errors / reference_chars if reference_chars else 0.0
    polished_cer = polished_errors / reference_chars if reference_chars else 0.0
    print("\n=== Character error summary (punctuation/space ignored) ===")
    print(f"Baseline: {base_errors}/{reference_chars} = {base_cer:.2%}")
    print(f"Polished: {polished_errors}/{reference_chars} = {polished_cer:.2%}")
    print(f"Samples: improved={improved}, same-score={unchanged_score}, regressed={regressed}")
    for result in results:
        if result["baseline"] != result["polished"]:
            print(f"\n[{result['outcome']}] {result['audio']}")
            print(f"  REF: {result['reference']}")
            print(f"  ASR: {result['baseline']}")
            print(f"  LLM: {result['polished']}")

    report = {
        "manifest": str(args.manifest), "samples": len(results),
        "baseline_errors": base_errors, "polished_errors": polished_errors,
        "reference_chars": reference_chars, "baseline_cer": base_cer,
        "polished_cer": polished_cer, "improved": improved,
        "same_score": unchanged_score, "regressed": regressed, "results": results,
    }
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                    encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
