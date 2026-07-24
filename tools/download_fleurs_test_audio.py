#!/usr/bin/env python3
"""Download a small punctuated FLEURS ASR test set.

The script streams only the beginning of each FLEURS test tarball, extracts a few
WAV files, and writes a JSONL manifest with punctuated transcripts from test.tsv.
"""

import argparse
import csv
import io
import json
import tarfile
from pathlib import Path

import requests
from scipy.io import wavfile

DATASET = "google/fleurs"
BASE = "https://huggingface.co/datasets/google/fleurs/resolve/main/data"
LANGS = {
    "zh": {"config": "cmn_hans_cn", "name": "Chinese Mandarin"},
    "en": {"config": "en_us", "name": "English"},
    "yue": {"config": "yue_hant_hk", "name": "Cantonese"},
    "ja": {"config": "ja_jp", "name": "Japanese"},
}


def fetch_tsv(config: str):
    url = f"{BASE}/{config}/test.tsv"
    response = requests.get(url, timeout=60)
    response.raise_for_status()
    rows = {}
    reader = csv.reader(io.StringIO(response.text), delimiter="\t")
    for row in reader:
        if len(row) < 7:
            continue
        rows[row[1]] = {
            "raw_transcription": row[2],
            "normalized_transcription": row[3],
            "duration_samples": int(row[5]),
            "gender": row[6],
        }
    return rows


def has_punctuation(text: str) -> bool:
    punctuation = set(".,;:!?()[]{}\"'，。！？；：（）「」『』、·—-–")
    return any(ch in punctuation for ch in text)


def convert_to_pcm16_mono(path: Path) -> None:
    sample_rate, data = wavfile.read(path)
    if sample_rate != 16000:
        raise RuntimeError(f"unexpected sample rate for {path}: {sample_rate}")
    if getattr(data, "ndim", 1) != 1:
        data = data[:, 0]
    if data.dtype.kind == "f":
        data = (data.clip(-1.0, 1.0) * 32767.0).astype("int16")
    elif data.dtype != "int16":
        max_abs = max(abs(int(data.min())), abs(int(data.max())), 1)
        data = (data.astype("float32") / max_abs * 32767.0).astype("int16")
    wavfile.write(path, 16000, data)


def extract_samples(lang: str, config: str, transcripts: dict, out_dir: Path, count: int):
    url = f"{BASE}/{config}/audio/test.tar.gz"
    response = requests.get(url, stream=True, timeout=60)
    response.raise_for_status()
    response.raw.decode_content = True

    lang_dir = out_dir / lang
    lang_dir.mkdir(parents=True, exist_ok=True)
    samples = []
    with tarfile.open(fileobj=response.raw, mode="r|gz") as tar:
        for member in tar:
            if not member.isfile() or not member.name.endswith(".wav"):
                continue
            filename = Path(member.name).name
            meta = transcripts.get(filename)
            if not meta or not has_punctuation(meta["raw_transcription"]):
                continue
            sample_index = len(samples)
            output_name = f"{lang}_{sample_index:02d}_{filename}"
            output_path = lang_dir / output_name
            src = tar.extractfile(member)
            if src is None:
                continue
            with output_path.open("wb") as f:
                f.write(src.read())
            convert_to_pcm16_mono(output_path)
            samples.append(
                {
                    "language": lang,
                    "config": config,
                    "dataset": DATASET,
                    "split": "test",
                    "audio": str(output_path),
                    "source_audio_name": filename,
                    **meta,
                }
            )
            if len(samples) >= count:
                break
    if len(samples) < count:
        raise RuntimeError(f"only found {len(samples)} samples for {lang}/{config}")
    return samples


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="testdata/fleurs")
    parser.add_argument("--count", type=int, default=5)
    args = parser.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out_dir / "manifest.jsonl"
    all_samples = []

    for lang, info in LANGS.items():
        config = info["config"]
        print(f"Downloading {lang} ({config})...")
        transcripts = fetch_tsv(config)
        all_samples.extend(extract_samples(lang, config, transcripts, out_dir, args.count))

    with manifest_path.open("w", encoding="utf-8") as f:
        for sample in all_samples:
            f.write(json.dumps(sample, ensure_ascii=False) + "\n")

    readme = out_dir / "README.md"
    readme.write_text(
        "# FLEURS punctuated ASR smoke-test audio\n\n"
        "Source: https://huggingface.co/datasets/google/fleurs\n\n"
        "Languages: Chinese Mandarin (`cmn_hans_cn`), English (`en_us`), "
        "Cantonese (`yue_hant_hk`), Japanese (`ja_jp`).\n\n"
        "Use `manifest.jsonl` for punctuated reference transcripts.\n",
        encoding="utf-8",
    )

    print(f"Wrote {len(all_samples)} samples to {out_dir}")
    print(f"Manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
