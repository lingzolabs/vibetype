# IBus Frontend

## Configuration

IBus frontend settings are stored in:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/vibetype/ibus.conf
```

You can open the setup dialog with:

```bash
/usr/bin/vibetype-ibus --setup
```

Or edit the config file manually:

```ini
[ibus]
TriggerKey=F12
SegmentSeconds=20
SocketPath=
AudioDevice=default
```

`TriggerKey` uses IBus key-string syntax. Examples:

- `F12`
- `Control+F12`
- `Alt+space`
- `Super+space`

When `SocketPath` is empty, the frontend uses
`$XDG_RUNTIME_DIR/vibetype/vibetype.sock`.

Command-line flags still override config-file values.

## Run without IBus integration

Start a fake backend:

```bash
./build/bin/vibetype-backend --fake-asr -s /tmp/vibetype.sock
```

Submit a generated silent WAV through the real frontend client code:

```bash
./frontend/ibus/vibetype_ibus.py \
  --socket /tmp/vibetype.sock \
  --test-silence-ms 500 \
  --no-wait-model
```

Expected output:

```text
COMMIT: fake transcript segment 0
```

Submit a prepared WAV:

```bash
./frontend/ibus/vibetype_ibus.py \
  --socket /tmp/vibetype.sock \
  --test-wav testdata/fleurs/zh/zh_00_10026684690566417990.wav \
  --no-wait-model
```

Test sound-card capture only, without backend:

```bash
./frontend/ibus/vibetype_ibus.py \
  --record-only 3 \
  --record-out /tmp/vibetype-record-test.wav
```

Expected output includes WAV stats. `peak` and `rms` should be above zero when the microphone captures sound.

Record once from ALSA default input, then submit to the backend:

```bash
./frontend/ibus/vibetype_ibus.py \
  --socket /tmp/vibetype.sock \
  --test-record 3 \
  --no-wait-model
```

The frontend reads raw PCM from `arecord -D <audio-device> -f S16_LE -r 16000 -c 1 -t raw`, writes 16 kHz mono PCM16 WAV, then sends the WAV path to the backend.

## Test with real backend model

Start backend with a local model:

```bash
./build/bin/vibetype-backend \
  -s /tmp/vibetype.sock \
  --model /path/to/sensevoice-small-q8.gguf \
  -t 4
```

Run the frontend test client and wait until the model is ready:

```bash
./frontend/ibus/vibetype_ibus.py \
  --socket /tmp/vibetype.sock \
  --test-wav testdata/fleurs/zh/zh_00_10026684690566417990.wav
```

## Run as IBus engine manually

Start the backend first, then run:

```bash
./frontend/ibus/vibetype_ibus.py --ibus --socket /tmp/vibetype.sock
```

The configured trigger key toggles recording. During recording, IBus shows a dynamic preedit/auxiliary indicator in the current input context. Partial results are displayed as status text only, and final text is committed to the focused application.

## Installed IBus component

The package installs:

```text
/usr/bin/vibetype-ibus
/usr/share/ibus/component/vibetype.xml
```

The component setup action launches:

```bash
/usr/bin/vibetype-ibus --setup
```

After installation, restart IBus or reload components:

```bash
ibus restart
```

Then add “Vibetype Voice Input” from the IBus input method settings.
