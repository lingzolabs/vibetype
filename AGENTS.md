# AGENTS.md

## Project Summary

- Treat this repository as Vibetype, a Linux voice input method for local offline ASR.
- Keep the backend frontend-agnostic; expose JSON-RPC over a Unix socket for CLI, IBus, and Fcitx5 frontends.
- Prioritize Chinese and English recognition quality while preserving mixed Chinese, English, Cantonese, Japanese, and Korean support.
- Follow `README.md` (Chinese), `README.en.md` (English), and `docs/vibetype-spec.md` as product, protocol, and packaging references.

## Tech Stack

- Use C++17 for `vibetype-backend` and the SenseVoice GGUF ASR engine.
- Use `xtils` for IPC, JSON-RPC, config, signal handling, and logging; fetch it from `https://github.com/lingzolabs/xtils.git` unless `VIBETYPE_USE_SYSTEM_XTILS=ON` is set.
- Use `ggml` from a `llama.cpp` source release zip; do not switch to a git clone without approval.
- Use Python 3 for CLI, IBus, and shared frontend client code.
- Use ALSA `arecord` for capture; accept `--audio-device` and default to ALSA `default`.
- Use 16 kHz mono PCM16 WAV as the only frontend-to-backend audio format.

## Directory Structure

- `src/backend/`: C++ backend daemon, JSON-RPC methods, SenseVoice engine, model download/status handling.
- `frontend/common/vibetype_client.py`: Shared JSON-RPC client, ALSA segmentation, session control.
- `frontend/cli/vibetype_cli.py`: CLI frontend, selectable sound card, hold-key mode, clipboard/paste output.
- `frontend/ibus/vibetype_ibus.py`: IBus frontend.
- `packaging/`: systemd user service and input-method component files.
- `docs/`: install, CLI, IBus, Fcitx5 status, and protocol/spec docs.
- `testdata/`: FLEURS test audio and manifests.
- `tools/`: utility scripts for test audio and fake clients.

## Key Commands

- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
- Build backend: `cmake --build build --target vibetype-backend -j$(nproc)`
- Build packages: `cmake --build build --target package -j$(nproc)`
- Check Python syntax: `python -m py_compile frontend/common/vibetype_client.py frontend/cli/vibetype_cli.py frontend/ibus/vibetype_ibus.py`
- Run fake backend: `./build/bin/vibetype-backend --fake-asr -s /tmp/vibetype.sock`
- Run CLI smoke test: `./frontend/cli/vibetype_cli.py --socket /tmp/vibetype.sock --duration 1 --segment-seconds 5 --input-method stdout --print-final`
- List ALSA devices: `./frontend/cli/vibetype_cli.py --list-audio-devices`
- Run real backend: `./build/bin/vibetype-backend -s /tmp/vibetype.sock --model ~/.config/vibetype/models/sensevoice-small-q8.gguf -t 4`

## Code Conventions

- Keep frontend and backend responsibilities separate: frontends capture, segment, display status, and commit final text; backend validates, transcribes, normalizes, aggregates, and emits results.
- Commit only `vibetype.finalResult`; treat partial results as display/status only.
- Validate every segment as 16 kHz mono PCM16 WAV before ASR.
- Keep model files under user config by default: `${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf`.
- Start backend IPC even when the model is missing; download/load the model in the background and report model status.
- Normalize final transcript text in the backend so CLI, IBus, and Fcitx5 behave consistently.
- Use `LogI`, `LogW`, `LogE`, and xtils logging for backend output; route third-party library logs into xtils where possible.
- Do not modify reference upstream code or vendored dependency sources unless explicitly requested.

## Frontend Behavior

- Default trigger key is F12.
- In CLI hold-key mode, treat each press/release pair as one independent recognition session; do not accumulate results across presses.
- Do not read the sound card while the hold key is released.
- Deliver CLI text through clipboard/paste by default; avoid character-by-character typing for multilingual text.
- Support `--input-method stdout` for deterministic tests.

## Packaging Notes

- Keep package variants as `vibetype`, `vibetype-ibus`, and `vibetype-fcitx5`.
- Make each package variant standalone and mutually exclusive; do not make `vibetype-ibus` or `vibetype-fcitx5` depend on `vibetype`.
- Put backend, CLI executable, shared Python, systemd service, and core docs in all package variants.
- Add IBus executable/component and IBus docs only to `vibetype-ibus`.
- Add the Fcitx5 addon library, input-method metadata, helper, and Fcitx5 docs only to `vibetype-fcitx5`.
- Generate `.tar.gz`, `.deb`, and `.rpm` packages with CPack.

## Workflow Tips

- Read `docs/vibetype-spec.md` before changing protocol behavior.
- Prefer small, verifiable changes and run the nearest smoke test after each change.
- Use `--fake-asr` for protocol/frontend work and a real SenseVoice model only when ASR behavior matters.
- Document benchmark numbers as upstream references; do not present them as guaranteed local results.
- Note that native VAD before ASR is planned but not currently implemented.
