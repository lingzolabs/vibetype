# Vibetype CLI

`vibetype-cli` records from an ALSA input device, sends live WAV segments to `vibetype-backend`,
waits for `vibetype.finalResult`, then delivers the final text. If no device is specified, it
uses ALSA `default`.

## Start backend

For protocol testing (fake ASR, no model needed):

```bash
./build/bin/vibetype-backend --fake-asr -s /tmp/vibetype.sock
```

For real ASR:

```bash
./build/bin/vibetype-backend \
  -s /tmp/vibetype.sock \
  --model /path/to/sensevoice-small-q8.gguf \
  -t 4
```

## Record until Ctrl+C

```bash
./frontend/cli/vibetype_cli.py \
  --socket /tmp/vibetype.sock \
  --segment-seconds 5 \
  --input-method stdout \
  --print-final
```

The CLI reads raw PCM from ALSA:

```bash
arecord -D default -f S16_LE -r 16000 -c 1 -t raw
```

List available ALSA devices:

```bash
vibetype-cli --list-audio-devices
```

Use a specific sound card / PCM device:

```bash
vibetype-cli --audio-device plughw:1,0
vibetype-cli --audio-device hw:1,0
vibetype-cli --audio-device default
```

Segments are written as 16 kHz mono PCM16 WAV files under
`${XDG_RUNTIME_DIR}/vibetype/<session>/`, sent to the backend as each one completes, and
finalized on Ctrl+C.

## Record fixed duration

```bash
./frontend/cli/vibetype_cli.py \
  --socket /tmp/vibetype.sock \
  --audio-device default \
  --duration 8 \
  --segment-seconds 4 \
  --input-method stdout
```

## Hold a key to record

Use evdev key events: press the key to start one independent recognition, release it to stop
audio capture, finalize that recognition, and output only that result. Press again for a new
independent recognition. The CLI does **not** read the sound card while the key is released.

```bash
./frontend/cli/vibetype_cli.py \
  --socket /tmp/vibetype.sock \
  --audio-device plughw:1,0 \
  --hold-key F12 \
  --segment-seconds 5 \
  --input-method auto
```

If the default scan cannot read input devices, specify the keyboard event device:

```bash
./frontend/cli/vibetype_cli.py \
  --socket /tmp/vibetype.sock \
  --hold-key KEY_F12 \
  --input-device /dev/input/eventX
```

Notes:

- `--hold-key` uses Linux evdev and may require the user to be in the `input` group or
  to run with sufficient permissions.
- Supported names include `F12`, `KEY_F12`, `SPACE`, `RIGHTCTRL`, `LEFTCTRL`, or a numeric
  evdev key code such as `88`.
- Each press/release pair is a separate backend session; results are **not** accumulated
  across presses.

## Deliver final text

Use clipboard delivery by default. This is safer for multilingual text than typing through
key-simulation tools. `auto` copies the final text to the clipboard and then tries to press
Ctrl+V automatically.

```bash
vibetype-cli --input-method auto
```

Selection order:

1. Copy text to clipboard via `wl-copy` on Wayland or `xclip` on X11.
2. Try to paste by sending Ctrl+V (`xdotool`/`ydotool` only send the hotkey, not the text).
3. If auto-paste is unavailable, leave text in clipboard and ask you to paste manually.
4. `stdout` fallback if clipboard is unavailable.

Use an explicit method when needed:

```bash
vibetype-cli --input-method paste      # copy, then try Ctrl+V
vibetype-cli --input-method clipboard  # copy only
vibetype-cli --input-method stdout
```

Notes:

- `auto`/`paste` never type the multilingual text character by character; text goes through
  the clipboard.
- Auto-paste depends on the desktop allowing a helper to send Ctrl+V. If blocked, paste
  manually with Ctrl+V.
- `clipboard` copies the final text only.
- `stdout` is always safe for debugging and automated tests.

## Config reload diagnostics

The following commands are intended for **testing and diagnostics only**. Under normal
operation the backend reloads `backend.json` / `text-processing.json` automatically via
mtime polling (every ~5 seconds), on every `startSession`, or immediately on SIGHUP.

Force a config reload and show the result:

```bash
# Using the fake backend with a custom socket
vibetype-backend --fake-asr -s /tmp/vibetype.sock &
# Send reloadConfig via the Python client helper (or any JSON-RPC client)
python3 -c "
import socket, json
msg = json.dumps({'jsonrpc':'2.0','id':1,'method':'vibetype.reloadConfig','params':{}})
# ... send over Unix socket /tmp/vibetype.sock
"
```

Show current config status:

```bash
# vibetype.configStatus returns loaded_at, revision, enable_builtin_corrections,
# enable_qwen_polish, custom_corrections_count, and last error if any.
```

For a quick SIGHUP test:

```bash
pkill -HUP vibetype-backend
# Backend reloads within 100 ms and logs: "SIGHUP received — reloading config"
```
