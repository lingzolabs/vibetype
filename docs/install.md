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

Generate split Debian and generic tar packages with CPack:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --target package -j$(nproc)
```

Outputs include:

```text
# Core package: backend + CLI + shared Python + systemd user service
build/vibetype_0.1.0_*.deb
build/vibetype-0.1.0-*.tar.gz

# IBus add-on: install together with vibetype
build/vibetype-ibus_0.1.0_*.deb
build/vibetype-ibus-addon-0.1.0-*.tar.gz

# Fcitx5 add-on: install together with vibetype
build/vibetype-fcitx5_0.1.0_*.deb
build/vibetype-fcitx5-addon-0.1.0-*.tar.gz
```

Use the `.deb` packages on Debian/Ubuntu-like systems:

```bash
sudo apt install ./build/vibetype_0.1.0_*.deb
sudo apt install ./build/vibetype-ibus_0.1.0_*.deb      # optional IBus frontend
sudo apt install ./build/vibetype-fcitx5_0.1.0_*.deb    # optional Fcitx5 frontend
```

Use the `.tar.gz` packages on Arch Linux or other distributions. On Arch Linux install dependencies first:

```bash
sudo pacman -S --needed curl python alsa-utils wl-clipboard xclip xdotool
sudo pacman -S --needed python-gobject ibus   # optional for IBus
sudo pacman -S --needed fcitx5                # optional for Fcitx5
```

Then extract the core package and the frontend add-on you want:

```bash
tar -tf build/vibetype-0.1.0-*.tar.gz
sudo tar -C / -xzf build/vibetype-0.1.0-*.tar.gz
sudo tar -C / -xzf build/vibetype-ibus-addon-0.1.0-*.tar.gz    # optional
sudo tar -C / -xzf build/vibetype-fcitx5-addon-0.1.0-*.tar.gz  # optional
systemctl --user daemon-reload
systemctl --user enable --now vibetype-backend.service
ibus restart    # when using IBus
fcitx5 -rd      # when using Fcitx5
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

See `ibus-frontend.md` for no-IBus protocol tests and manual IBus engine testing.

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
