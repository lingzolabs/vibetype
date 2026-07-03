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

By default, the backend uses:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/models/sensevoice-small-q8.gguf
```

If the model file is missing, the backend still starts its JSON-RPC socket and downloads the model from Hugging Face in the background. Frontends should call `vibetype.modelStatus` or watch `vibetype.modelStatusChanged` and display “model downloading/loading” instead of starting transcription.

Override the model path or URL manually:

```bash
vibetype-backend \
  --model ~/.config/vibetype/models/sensevoice-small-q8.gguf \
  --model-url https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF/resolve/main/sensevoice-small-q8.gguf
```

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

See `ibus-frontend.md` for configuration, no-IBus protocol tests, and manual IBus engine testing.

## Fcitx5 frontend

The package installs:

```text
/usr/bin/vibetype-fcitx5-helper
/usr/lib/fcitx5/vibetype.so
/usr/share/fcitx5/addon/vibetype.conf
/usr/share/fcitx5/inputmethod/vibetype-inputmethod.conf
```

See `fcitx5-frontend.md` for setup, configuration, and debugging.

## CLI frontend

The package installs:

```text
/usr/bin/vibetype-cli
```

Use it to record from the sound card, send live segments to the backend, and input the final text. See `cli.md`.
