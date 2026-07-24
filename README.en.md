# Vibetype

[🇨🇳 简体中文](README.md) | 🇬🇧 English

Vibetype is a Linux voice input method focused on **local, offline ASR**. It runs a SenseVoice Small GGUF model on CPU, captures microphone audio from ALSA, and delivers final text through CLI or input-method frontends.

> Current priority: improve Chinese and English recognition quality first, while keeping multilingual mixed recognition available.

## Highlights

- **Local and offline**: audio stays on your machine after the model is downloaded.
- **No GPU dependency**: CPU inference via `ggml` / `llama.cpp` components.
- **Multilingual mixed recognition**: Chinese, English, Cantonese, Japanese, and Korean.
- **Good for mixed Chinese/English speech**: punctuation normalization prefers full-width
  punctuation when CJK text is present, half-width otherwise; decimal points in numbers
  are preserved correctly.
- **Built-in computer term corrections**: a bundled `computer_terms.json` list fixes common
  ASR capitalization errors (e.g. `github` → `GitHub`, `javascript` → `JavaScript`).
  Enabled by default; disable with `"enable_builtin_corrections": false`.
- **Custom correction rules**: add a `custom_corrections` section (object or array format)
  to `text-processing.json`; user rules take priority over the built-in term list.
- **Optional Qwen3 final polishing**: Qwen3-0.6B Q4\_K\_M GGUF, **off by default**; when
  enabled and the model is loaded, the final text is polished before delivery; on failure
  the backend falls back to rule-based corrections silently.
- **Config hot-reload**: edit `backend.json` or `text-processing.json` and the backend
  picks up changes within 5 seconds (mtime polling), on the next recording session
  (`startSession` mtime check), or immediately on SIGHUP — no restart needed.
- **Frontend-agnostic backend**: JSON-RPC over Unix socket, built with `xtils::IpcServer`.
- **Package split**:
  - `vibetype`: backend + CLI + shared Python code + systemd user service.
  - `vibetype-ibus`: IBus frontend add-on.
  - `vibetype-fcitx5`: Fcitx5 frontend add-on.

## Frontend status

| Frontend | Status | Notes |
| --- | --- | --- |
| CLI | Available | ALSA capture, selectable sound card, push-to-talk key mode, clipboard paste delivery. |
| IBus | Available | Commits only final text; partial text is status/preedit display only. |
| Fcitx5 | Available | C++ addon frontend; commits only final text. |

## Performance and accuracy references

The default target model is **SenseVoiceSmall Q8 GGUF**. The following numbers are upstream /
public benchmark references to illustrate model capability, **not a guarantee for every
microphone, accent, or desktop environment**. Vibetype currently runs local CPU ASR and will
continue to add features such as VAD and domain adaptation.

| Source / setup | Metric | Result |
| --- | --- | --- |
| SenseVoice README | Latency | SenseVoice-Small reports about **70 ms for 10 s audio**, described as **15× faster than Whisper-Large**. |
| SenseVoice llama.cpp / GGUF benchmark, Mandarin, CPU 8 threads, model-load excluded | Speed | SenseVoiceSmall around **20× real-time**. Equivalent **RTF ≈ 0.05**. |
| Same benchmark, 184 Mandarin clips, micro-CER with `normalize_zh` | Accuracy | SenseVoiceSmall **7.81% CER** fp32 reference / **8.17% CER** Q8 runtime. |
| Same benchmark, whisper.cpp comparison | Accuracy | whisper.cpp base **31.33% CER**, small **22.12% CER**, large-v3-turbo **23.15% CER** on the same Mandarin benchmark. |

Notes:

- Lower **CER/WER** is better; lower **RTF** is faster. RTF 0.05 means roughly 20 seconds
  of audio processed per 1 second of compute.
- The quoted Mandarin GGUF benchmark uses a VAD-based segmentation pipeline. Vibetype's
  current backend does not yet use VAD before ASR, so real-world long-form accuracy may
  differ until native VAD is added.
- The first optimization target for Vibetype is **Chinese and English** recognition quality.
  Fine-tuning / domain adaptation is planned for user vocabulary, names, and technical terms.

References:

- SenseVoice README: <https://github.com/FunAudioLLM/SenseVoice>
- GGUF CPU benchmark: <https://github.com/FunAudioLLM/SenseVoice/blob/main/runtime/llama.cpp/BENCHMARKS.md>

## Models

### SenseVoice model

Default model path:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf
```

If the model is missing, the backend starts its IPC socket first, downloads the model in the
background, and reports model status to frontends via `vibetype.modelStatusChanged`.

### Qwen3 polish model (optional)

The backend optionally uses **Qwen3-0.6B Q4\_K\_M GGUF** to polish the final ASR text.
This feature is **disabled by default** and the model is **not downloaded automatically**
unless you enable it.

Default path when enabled:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/qwen3-0.6b-q4_k_m.gguf
```

The installed default `/usr/share/vibetype/text-processing.json` contains the base prompt.
The user file `~/.config/vibetype/text-processing.json` only needs to override selected fields:

```json
{
  "enable_qwen_polish": true
}
```

A user `qwen_system_prompt` overrides the default prompt; changing it requires a config
reload, not a rebuild. If neither layer supplies a valid prompt, the backend reports a
configuration error, skips LLM processing, and preserves the rule-based result. Model
download, inference failure, and timeout behavior remain fail-safe.

Model sources:

- SenseVoice: <https://github.com/FunAudioLLM/SenseVoice>
- SenseVoice Small GGUF: <https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF>
- Qwen3-0.6B GGUF: <https://huggingface.co/Qwen/Qwen3-0.6B-GGUF>

## Text processing configuration

The backend reads text processing settings from two optional JSON files:

| File | Default path |
|------|-------------|
| `backend.json` | `~/.config/vibetype/backend.json` |
| `text-processing.json` | `~/.config/vibetype/text-processing.json` |

Both files are optional; `text-processing.json` takes precedence over `backend.json` for
text-processing fields.

### Built-in computer term corrections

Enabled by default. To disable:

```json
{ "enable_builtin_corrections": false }
```

### Custom correction rules

The `custom_corrections` field accepts either an object or an array (max 500 rules; key and
value each at most 256 bytes). User rules take priority over built-in terms.

```json
{
  "custom_corrections": [
    { "from": "维博太普", "to": "Vibetype"   },
    { "from": "吉特哈布", "to": "GitHub"     },
    { "from": "k8s",      "to": "Kubernetes" }
  ]
}
```

### Config hot-reload

The backend picks up changes to `backend.json` and `text-processing.json` automatically,
without restarting:

- **Every `startSession`**: mtime check, reload if changed.
- **Every ~5 seconds**: main loop polls mtime automatically.
- **SIGHUP** (within 100 ms): `pkill -HUP vibetype-backend`
- **`vibetype.reloadConfig` RPC**: for testing and diagnostics.

On successful reload the backend broadcasts `vibetype.statusChanged` with an incremented
`revision` number.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target package -j$(nproc)
```

Dependencies are fetched by CMake where appropriate:

- `xtils` from <https://github.com/lingzolabs/xtils>
- `llama.cpp` / `ggml` from a source release zip

To use a system-installed `xtils` instead:

```bash
cmake -B build -DVIBETYPE_USE_SYSTEM_XTILS=ON
```

## Packages

CPack generates standalone package variants:

```text
vibetype-*.tar.gz
vibetype-ibus-*.tar.gz
vibetype-fcitx5-*.tar.gz

vibetype_*.deb
vibetype-ibus_*.deb
vibetype-fcitx5_*.deb

vibetype-*.rpm
vibetype-ibus-*.rpm
vibetype-fcitx5-*.rpm
```

These three package variants are mutually exclusive. Each one is a full
package and includes its own backend/runtime files.

Install one tar package variant:

```bash
sudo tar -C / -xzf build/vibetype-*.tar.gz
systemctl --user daemon-reload
systemctl --user enable --now vibetype-backend.service
```

Or install the IBus / Fcitx5 variants:

```bash
sudo tar -C / -xzf build/vibetype-ibus-*.tar.gz
ibus restart

sudo tar -C / -xzf build/vibetype-fcitx5-*.tar.gz
fcitx5 -rd
```

On RPM-based systems, install one matching `.rpm` variant instead.

## CLI usage

List ALSA devices:

```bash
vibetype-cli --list-audio-devices
```

Record from the default microphone until Ctrl+C:

```bash
vibetype-cli --audio-device default --input-method auto
```

Record from a selected sound card:

```bash
vibetype-cli --audio-device plughw:1,0 --input-method auto
```

Push-to-talk style: each press/release pair is one independent recognition result:

```bash
vibetype-cli --hold-key F12 --audio-device default --input-method auto
```

`auto` copies recognized text to the clipboard and tries to paste with Ctrl+V. This avoids
character-by-character key simulation for multilingual text.

## Backend protocol

The backend exposes JSON-RPC methods over a Unix socket:

- `vibetype.hello`
- `vibetype.startSession`
- `vibetype.transcribeSegment`
- `vibetype.finishSession`
- `vibetype.cancelSession`
- `vibetype.modelStatus`
- `vibetype.reloadConfig` (testing / diagnostics)
- `vibetype.configStatus` (testing / diagnostics)

Notifications:

- `vibetype.partialResult`
- `vibetype.finalResult`
- `vibetype.error`
- `vibetype.modelStatusChanged`
- `vibetype.statusChanged`

## Documentation

- CLI: [`docs/cli.md`](docs/cli.md)
- IBus: [`docs/ibus-frontend.md`](docs/ibus-frontend.md)
- Install: [`docs/install.md`](docs/install.md)
- Spec: [`docs/vibetype-spec.md`](docs/vibetype-spec.md)
- Fcitx5: [`docs/fcitx5-frontend.md`](docs/fcitx5-frontend.md)

## License

MIT License. See [LICENSE](LICENSE) for details.
