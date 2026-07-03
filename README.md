# Vibetype

Vibetype is a Linux voice input method focused on **local, offline ASR**. It runs a SenseVoice Small GGUF model on CPU, captures microphone audio from ALSA, and delivers final text through CLI or input-method frontends.

> Current priority: improve Chinese and English recognition quality first, while keeping multilingual mixed recognition available.

## Highlights

- **Local and offline**: audio stays on your machine after the model is downloaded.
- **No GPU dependency**: CPU inference via `ggml` / `llama.cpp` components.
- **Fast and accurate**: SenseVoiceSmall GGUF benchmark references report about **20× real-time** CPU inference and **8.17% Mandarin CER** for the Q8 runtime.
- **Multilingual mixed recognition**: Chinese, English, Cantonese, Japanese, and Korean.
- **Good for mixed Chinese/English speech**: punctuation normalization prefers full-width punctuation when CJK text is present, and half-width punctuation otherwise.
- **Frontend-agnostic backend**: JSON-RPC over Unix socket, built with `xtils::IpcServer`.
- **Package split**:
  - `vibetype-cli`: backend + CLI + shared Python code + systemd user service.
  - `vibetype-ibus`: IBus frontend add-on.
  - `vibetype-fcitx5`: Fcitx5 add-on package name reserved; frontend implementation is pending.

## Frontend status

| Frontend | Status | Notes |
| --- | --- | --- |
| CLI | Available | ALSA capture, selectable sound card, push-to-talk key mode, clipboard paste delivery. |
| IBus | Available | Commits only final text; partial text is status/preedit style display. |
| Fcitx5 | Planned | Package split is prepared; frontend implementation is pending. |

## Performance and accuracy references

The default target model is **SenseVoiceSmall Q8 GGUF**. The following numbers are upstream / public benchmark references, not a promise for every microphone, accent, or desktop environment. Vibetype currently runs local CPU ASR and will continue to add frontend/backend polish such as VAD and domain adaptation.

| Source / setup | Metric | Result |
| --- | --- | --- |
| SenseVoice README | Latency | SenseVoice-Small reports about **70 ms for 10 s audio**, described as **15× faster than Whisper-Large**. |
| SenseVoice llama.cpp / GGUF benchmark, Mandarin, CPU 8 threads, model-load excluded | Speed | SenseVoiceSmall around **20× real-time**. Equivalent **RTF ≈ 0.05**. |
| Same benchmark, 184 Mandarin clips, micro-CER with `normalize_zh` | Accuracy | SenseVoiceSmall **7.81% CER** fp32 reference / **8.17% CER** Q8 runtime. |
| Same benchmark, whisper.cpp comparison | Accuracy | whisper.cpp base **31.33% CER**, small **22.12% CER**, large-v3-turbo **23.15% CER** on the same Mandarin benchmark. |

Notes:

- Lower **CER/WER** is better; lower **RTF** is faster. RTF 0.05 means roughly 20 seconds of audio processed per 1 second of compute.
- The quoted Mandarin GGUF benchmark uses a VAD-based segmentation pipeline. Vibetype's current backend does not yet use VAD before ASR, so real-world long-form accuracy may differ until native VAD is added.
- The first optimization target for Vibetype is **Chinese and English** recognition quality. Fine-tuning / domain adaptation is planned later for user-specific vocabulary, names, technical terms, and mixed Chinese-English dictation.

References:

- SenseVoice README: <https://github.com/FunAudioLLM/SenseVoice>
- GGUF CPU benchmark: <https://github.com/FunAudioLLM/SenseVoice/blob/main/runtime/llama.cpp/BENCHMARKS.md>

## Model

Default model path:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf
```

If the model is missing, the backend starts its IPC socket first, downloads the model in the background, and reports model status to frontends.

Model source:

- SenseVoice: <https://github.com/FunAudioLLM/SenseVoice>
- SenseVoice Small GGUF: <https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF>
- Benchmark references: <https://www.funasr.com/en/blog/funasr-vs-whisper-benchmark.html>

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

CPack generates split packages:

```text
vibetype-cli-*.tar.gz
vibetype-ibus-addon-*.tar.gz
vibetype-fcitx5-addon-*.tar.gz

vibetype-cli_*.deb
vibetype-ibus_*.deb
vibetype-fcitx5_*.deb
```

Install CLI only:

```bash
sudo tar -C / -xzf build/vibetype-cli-*.tar.gz
systemctl --user daemon-reload
systemctl --user enable --now vibetype-backend.service
```

Install CLI + IBus:

```bash
sudo tar -C / -xzf build/vibetype-cli-*.tar.gz
sudo tar -C / -xzf build/vibetype-ibus-addon-*.tar.gz
systemctl --user daemon-reload
systemctl --user enable --now vibetype-backend.service
ibus restart
```

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

`auto` copies recognized text to the clipboard and tries to paste with Ctrl+V. This avoids character-by-character key simulation for multilingual text.

## Backend protocol

The backend exposes JSON-RPC methods over a Unix socket:

- `vibetype.hello`
- `vibetype.startSession`
- `vibetype.transcribeSegment`
- `vibetype.finishSession`
- `vibetype.cancelSession`
- `vibetype.modelStatus`

Notifications:

- `vibetype.partialResult`
- `vibetype.finalResult`
- `vibetype.error`
- `vibetype.modelStatusChanged`

## Documentation

- CLI: [`docs/cli.md`](docs/cli.md)
- IBus: [`docs/ibus-frontend.md`](docs/ibus-frontend.md)
- Install: [`docs/install.md`](docs/install.md)
- Spec: [`docs/vibetype-spec.md`](docs/vibetype-spec.md)
- Fcitx5 status: [`docs/fcitx5-frontend.md`](docs/fcitx5-frontend.md)

## License

TBD.
