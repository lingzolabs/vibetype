# Install and Run

## Install backend from source

Build and install into a system prefix:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target vibetype-backend -j$(nproc)
sudo cmake --install build
```

For a personal install, use `$HOME/.local`, copy the unit to `~/.config/systemd/user`, and change `ExecStart` to `%h/.local/bin/vibetype-backend`.

## Build packages

Generate split Debian, RPM, and tar packages with CPack:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target package -j$(nproc)
```

Outputs include:

```text
# Standalone tar packages (mutually exclusive)
build/vibetype-0.1.1-*.tar.gz
build/vibetype-ibus-0.1.1-*.tar.gz
build/vibetype-fcitx5-0.1.1-*.tar.gz

# Standalone Debian packages (mutually exclusive)
build/vibetype_0.1.1_*.deb
build/vibetype-ibus_0.1.1_*.deb
build/vibetype-fcitx5_0.1.1_*.deb

# Standalone RPM packages (mutually exclusive)
build/vibetype-0.1.1-*.rpm
build/vibetype-ibus-0.1.1-*.rpm
build/vibetype-fcitx5-0.1.1-*.rpm
```

These three package variants are mutually exclusive. Each one is a full
package and includes its own backend/runtime files.

Use the `.deb` packages on Debian/Ubuntu-like systems:

```bash
sudo apt install ./build/vibetype_0.1.1_*.deb
# or
sudo apt install ./build/vibetype-ibus_0.1.1_*.deb
# or
sudo apt install ./build/vibetype-fcitx5_0.1.1_*.deb
```

Use the `.rpm` packages on RPM-based systems:

```bash
sudo dnf install ./build/vibetype-0.1.1-*.rpm
# or
sudo dnf install ./build/vibetype-ibus-0.1.1-*.rpm
# or
sudo dnf install ./build/vibetype-fcitx5-0.1.1-*.rpm
```

Use one `.tar.gz` package variant on other distributions:

```bash
tar -tf build/vibetype-0.1.1-*.tar.gz
sudo tar -C / -xzf build/vibetype-0.1.1-*.tar.gz
systemctl --user daemon-reload
systemctl --user enable --now vibetype-backend.service
```

For the IBus / Fcitx5 tar variants:

```bash
sudo tar -C / -xzf build/vibetype-ibus-0.1.1-*.tar.gz
ibus restart

sudo tar -C / -xzf build/vibetype-fcitx5-0.1.1-*.tar.gz
fcitx5 -rd
```

## Enable systemd user service

Install the service file if CMake/package installation did not place it in the active user unit path:

```bash
mkdir -p ~/.config/systemd/user
cp packaging/systemd/vibetype-backend.service ~/.config/systemd/user/
```

Enable and start the backend:

```bash
systemctl --user daemon-reload
systemctl --user enable --now vibetype-backend.service
journalctl --user -u vibetype-backend.service -f
```

## Model location

### SenseVoice model

Default path:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf
```

If the model file is missing, the backend still starts its JSON-RPC socket and downloads the model
from Hugging Face in the background. Frontends should call `vibetype.modelStatus` or watch
`vibetype.modelStatusChanged` and display "model downloading/loading" instead of starting
transcription.

Override the model path or URL manually:

```bash
vibetype-backend \
  --model ~/.config/vibetype/models/sensevoice-small-q8.gguf \
  --model-url https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF/resolve/main/sensevoice-small-q8.gguf
```

### Qwen3 polish model (optional)

The backend optionally uses a **Qwen3-0.6B Q4\_K\_M GGUF** model for final-result polishing.
This feature is **disabled by default** and the model is **not downloaded automatically** unless
you enable it.

Default path when enabled:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/qwen3-0.6b-q4_k_m.gguf
```

The backend has no compiled-in prompt. The installed default
`/usr/share/vibetype/text-processing.json` supplies the base prompt, while the user file
only overrides selected fields. To enable polishing, create
`~/.config/vibetype/text-processing.json` with:

```json
{
  "enable_qwen_polish": true
}
```

After saving the file, the backend will hot-reload the config within 5 seconds (or on the
next recording session) and start loading the Qwen model. A user `qwen_system_prompt`
overrides the installed default and requires only a config reload. If neither layer
provides a prompt, polishing is skipped and `vibetype.polishStatus` reports an error. If
the model file is missing, the backend downloads it automatically from Hugging Face.

If Qwen inference fails or times out, the backend falls back to the rule-based correction
result and logs a warning. The final text is always delivered — Qwen is a best-effort enhancement.

## Text processing configuration

The backend reads text processing settings from two optional JSON files:

| File | Default path |
|------|-------------|
| `backend.json` | `~/.config/vibetype/backend.json` |
| `text-processing.json` | `~/.config/vibetype/text-processing.json` |

Both files are optional. `text-processing.json` takes precedence over `backend.json` for
text-processing fields.

### Built-in computer term corrections

The backend ships a built-in `computer_terms.json` list of common ASR misrecognitions and their
correct forms (e.g. `"github"` → `"GitHub"`, `"javascript"` → `"JavaScript"`). This is
**enabled by default**. To disable it:

```json
{
  "enable_builtin_corrections": false
}
```

### Custom correction rules

User-defined correction rules can be specified in `custom_corrections`. They take priority over
the built-in term list.

**Object format** (suitable for a small number of rules):

```json
{
  "custom_corrections": {
    "维博太普": "Vibetype",
    "吉特哈布": "GitHub",
    "k8s":      "Kubernetes"
  }
}
```

**Array format** (recommended for larger sets; easier to manage order):

```json
{
  "custom_corrections": [
    { "from": "维博太普",  "to": "Vibetype"    },
    { "from": "吉特哈布",  "to": "GitHub"      },
    { "from": "k8s",       "to": "Kubernetes"  }
  ]
}
```

Limits: maximum 500 rules; key and value each at most 256 bytes.

### Priority order

```
text-processing.json custom_corrections   (highest priority)
  > backend.json custom_corrections
  > built-in computer_terms.json          (lowest priority)
```

Built-in entries are only applied where no user-defined rule covers the same key.

### Minimal text-processing.json example

```json
{
  "enable_builtin_corrections": true,
  "enable_qwen_polish": false,
  "custom_corrections": [
    { "from": "维博太普", "to": "Vibetype" }
  ]
}
```

## Configuration hot-reload

The backend automatically picks up changes to `backend.json` and `text-processing.json`
without restarting. Hot-reload is triggered by any of the following:

1. **Every `startSession` call**: the backend checks both file mtimes and reloads if either
   has changed. This ensures config is always current before a new recording session begins.

2. **Periodic mtime polling (every ~5 seconds)**: the main loop polls file mtimes every 5
   seconds and reloads automatically on change.

3. **SIGHUP signal (immediate, within 100 ms)**:
   ```bash
   pkill -HUP vibetype-backend
   # or
   kill -HUP $(systemctl --user show vibetype-backend.service -p MainPID --value)
   ```

4. **`vibetype.reloadConfig` JSON-RPC call**: force-reloads and returns the new config
   status. Intended for testing and diagnostics, not normal operation.

After a successful reload the backend broadcasts `vibetype.statusChanged` with
`reason: "config_reloaded"` and an incremented `revision` number.

If a reload fails (e.g. JSON parse error), the previous config is kept intact,
an error is logged, and the same file will not be retried until its mtime changes again.

## Frontend behavior while model is not ready

When the user triggers recording before the model is ready, show a status message such as:

```text
Vibetype model is still downloading/loading. Please try again when it is ready.
```

Do not commit text until the backend emits `vibetype.finalResult`.

## IBus frontend

The package installs:

```text
/usr/bin/vibetype-ibus
/usr/share/ibus/component/vibetype.xml
```

IBus settings are stored in `${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/ibus.conf`.
Open the setup dialog with `vibetype-ibus --setup`.

See [`ibus-frontend.md`](ibus-frontend.md) for configuration, protocol testing, and manual
IBus engine testing.

## Fcitx5 frontend

The package installs:

```text
/usr/bin/vibetype-fcitx5-helper
/usr/lib/fcitx5/vibetype.so
/usr/share/fcitx5/addon/vibetype.conf
/usr/share/fcitx5/inputmethod/vibetype-inputmethod.conf
```

See [`fcitx5-frontend.md`](fcitx5-frontend.md) for setup, configuration, and debugging.

## CLI frontend

The package installs:

```text
/usr/bin/vibetype-cli
```

Use it to record from the sound card, send live segments to the backend, and input the final
text. See [`cli.md`](cli.md).
