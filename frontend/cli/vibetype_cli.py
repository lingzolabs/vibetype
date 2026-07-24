#!/usr/bin/env python3
"""Realtime sound-card CLI for Vibetype.

Records from ALSA default input, sends live WAV segments to vibetype-backend,
waits for the final result, then inputs the text with a selected commit method.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import select
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

# Allow running from the source tree and from installed /usr/bin.
SOURCE_COMMON_DIR = Path(__file__).resolve().parents[1] / "common"
INSTALLED_COMMON_DIR = Path(__file__).resolve().parents[1] / "share" / "vibetype" / "python"
for path in (SOURCE_COMMON_DIR, INSTALLED_COMMON_DIR, Path("/usr/share/vibetype/python")):
    if path.exists():
        sys.path.insert(0, str(path))

from vibetype_client import VibetypeController, default_socket_path  # noqa: E402
from vibetype_client import JsonRpcClient  # noqa: E402
EV_KEY = 0x01
KEY_CODES = {
    "KEY_SPACE": 57,
    "KEY_RIGHTCTRL": 97,
    "KEY_LEFTCTRL": 29,
    "KEY_F12": 88,
    "SPACE": 57,
    "RIGHTCTRL": 97,
    "LEFTCTRL": 29,
    "F12": 88,
}


def command_exists(name: str) -> bool:
    return shutil.which(name) is not None


def parse_key_code(value: str) -> int:
    upper = value.upper()
    if upper in KEY_CODES:
        return KEY_CODES[upper]
    if upper.startswith("KEY_") and upper in KEY_CODES:
        return KEY_CODES[upper]
    try:
        return int(value, 0)
    except ValueError as exc:
        known = ", ".join(sorted(KEY_CODES.keys()))
        raise ValueError(f"unknown key {value!r}; use numeric evdev code or one of: {known}") from exc


def list_audio_devices() -> int:
    print("# ALSA PCM device names usable with --audio-device", flush=True)
    subprocess.run(["arecord", "-L"], check=False)
    print("\n# Hardware capture devices", flush=True)
    subprocess.run(["arecord", "-l"], check=False)
    return 0


def open_input_devices(device: str | None):
    paths = [device] if device else sorted(glob.glob("/dev/input/event*"))
    files = []
    errors = []
    for path in paths:
        if not path:
            continue
        try:
            files.append(open(path, "rb", buffering=0))
        except OSError as exc:
            errors.append(f"{path}: {exc}")
    if not files:
        raise RuntimeError("no readable input event devices. " + "; ".join(errors[:3]))
    return files


# ── Key listener backends ─────────────────────────────────────────────


class KeyListenerEvdev:
    """Listen for key events via /dev/input/event* (requires input group)."""

    def __init__(self, key_code: int, device: str | None):
        self.key_code = key_code
        self.files = open_input_devices(device)
        self.event_struct = struct.Struct("llHHi")

    def run(self, on_press, on_release):
        try:
            while True:
                readable, _, _ = select.select(self.files, [], [])
                for f in readable:
                    data = os.read(f.fileno(), self.event_struct.size)
                    if len(data) != self.event_struct.size:
                        continue
                    _sec, _usec, ev_type, code, value = self.event_struct.unpack(data)
                    if ev_type != EV_KEY or code != self.key_code:
                        continue
                    if value == 1:
                        on_press()
                    elif value == 0:
                        on_release()
        finally:
            for f in self.files:
                f.close()


class KeyListenerTerminal:
    """Listen for key press/release in the terminal via raw stdin.

    Uses termios raw mode.  Supports single-char keys and common escape
    sequences (F1–F12, arrows, etc.).  Since terminals cannot distinguish
    physical press vs release, we treat each keypress as a press+hold and
    use a timeout to detect release (key-up).
    """

    # Mapping from escape sequences to friendly names
    _SEQ_MAP: dict[str, str] = {
        "\x1b[24~": "F12", "\x1bOP": "F1", "\x1bOQ": "F2",
        "\x1bOR": "F3", "\x1bOS": "F4", "\x1b[15~": "F5",
        "\x1b[17~": "F6", "\x1b[18~": "F7", "\x1b[19~": "F8",
        "\x1b[20~": "F9", "\x1b[21~": "F10", "\x1b[23~": "F11",
        " ": "SPACE",
    }

    # Reverse: name -> expected trigger
    _NAME_MAP: dict[int, str] = {
        88: "F12", 59: "F1", 60: "F2", 61: "F3", 62: "F4",
        63: "F5", 64: "F6", 65: "F7", 66: "F8", 67: "F9",
        68: "F10", 87: "F11",
        57: "SPACE", 29: "LEFTCTRL", 97: "RIGHTCTRL",
    }

    def __init__(self, key_code: int, release_timeout: float = 0.5):
        self.key_code = key_code
        self.release_timeout = release_timeout
        key_name = self._NAME_MAP.get(key_code)
        if not key_name:
            raise RuntimeError(
                f"terminal backend does not support key code {key_code}; "
                f"supported: {', '.join(f'{k}({v})' for k, v in sorted(self._NAME_MAP.items()))}"
            )
        self.key_name = key_name

    def _match(self, buf: str) -> bool:
        """Check if buffer matches our trigger key."""
        seq_name = self._SEQ_MAP.get(buf)
        return seq_name == self.key_name

    def run(self, on_press, on_release):
        import termios
        import tty

        fd = sys.stdin.fileno()
        old_settings = termios.tcgetattr(fd)
        try:
            tty.setraw(fd)
            holding = False
            while True:
                # Wait for input or timeout (release detection)
                readable, _, _ = select.select([fd], [], [], self.release_timeout if holding else None)
                if not readable:
                    # Timeout: key released
                    if holding:
                        holding = False
                        on_release()
                    continue

                ch = os.read(fd, 32).decode("utf-8", errors="ignore")
                if not ch:
                    break

                # Ctrl+C to quit
                if ch == "\x03":
                    if holding:
                        holding = False
                        on_release()
                    raise KeyboardInterrupt

                if self._match(ch):
                    if not holding:
                        holding = True
                        on_press()
                    # else: key repeat while holding, ignore
                else:
                    # Different key pressed while holding -> release + ignore
                    if holding:
                        holding = False
                        on_release()
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)


def create_key_listener(key_code: int, input_device: str | None, backend: str | None):
    """Create the best available key listener.

    Priority: explicit --key-backend > terminal > evdev.
    """
    if backend == "evdev":
        return KeyListenerEvdev(key_code, input_device)
    if backend == "terminal":
        return KeyListenerTerminal(key_code)

    # Auto-detect: try terminal first (no special permissions needed)
    if sys.stdin.isatty():
        try:
            return KeyListenerTerminal(key_code)
        except RuntimeError:
            pass

    # Fall back to evdev
    try:
        return KeyListenerEvdev(key_code, input_device)
    except RuntimeError:
        pass

    raise RuntimeError(
        "no key listener backend available.\n"
        "  • Terminal: run from an interactive terminal (default, no special perms)\n"
        "  • Evdev: add your user to the 'input' group, or use --input-device\n"
        "  • Use --key-backend to force one"
    )


def copy_to_clipboard(text: str) -> bool:
    clipboard_commands = []
    if command_exists("wl-copy"):
        clipboard_commands.append(["wl-copy", "--type", "text/plain;charset=utf-8"])
    if command_exists("xclip"):
        clipboard_commands.append(["xclip", "-selection", "clipboard", "-in"])

    for cmd in clipboard_commands:
        try:
            subprocess.run(cmd, input=text.encode("utf-8"), check=True)
            return True
        except (FileNotFoundError, subprocess.CalledProcessError) as exc:
            print(f"WARN: clipboard command failed: {' '.join(cmd)}: {exc}", file=sys.stderr)
    return False


def input_text(text: str, method: str) -> None:
    if not text:
        return

    if method in ("auto", "clipboard"):
        if copy_to_clipboard(text):
            sys.stderr.write(f"\r\x1b[K✓ {text}\r\n")
            sys.stderr.flush()
            return
        if method == "clipboard":
            sys.stderr.write("\r\x1b[KWARN: no clipboard tool; falling back to stdout\r\n")
            sys.stderr.flush()

    # stdout fallback
    sys.stdout.write(f"\r\x1b[K{text}\r\n")
    sys.stdout.flush()


class CliApp:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.final_text = ""
        self.controller = self._new_controller()

    def _new_controller(self) -> VibetypeController:
        return VibetypeController(
            self.args.socket,
            segment_seconds=self.args.segment_seconds,
            audio_device=self.args.audio_device,
            commit_callback=self._on_commit,
            status_callback=self._on_status,
            min_record_ms=0,  # terminal mode: no min duration, user explicitly triggers
        )

    def _reset_controller(self) -> None:
        # Keep the JSON-RPC connection alive across press/release sessions.
        # Only reset per-utterance state; close the socket only when the CLI exits.
        self.final_text = ""

    def _on_status(self, text: str) -> None:
        if not self.args.quiet:
            # Permanent messages (errors, cancellations, results) get their own line.
            # Transient status (recording, stopping, etc.) overwrites in-place.
            if text.startswith(("cancelled:", "error:", "final result ready")):
                sys.stderr.write(f"\r\x1b[K{text}\r\n")
            else:
                sys.stderr.write(f"\r\x1b[K{text}")
            sys.stderr.flush()

    def _clear_status_line(self) -> None:
        """Move past the status line before printing final output."""
        sys.stderr.write("\r\x1b[K")
        sys.stderr.flush()

    def _on_commit(self, text: str) -> None:
        self.final_text = text

    def _finish_and_input(self) -> int:
        if not self.controller.final_event.wait(timeout=self.args.final_timeout):
            self._clear_status_line()
            sys.stderr.write("ERROR: timed out waiting for finalResult\r\n")
            sys.stderr.flush()
            return 1
        self._clear_status_line()
        if not self.final_text:
            return 0  # empty text (cancelled session) — nothing to output
        if self.args.print_final:
            sys.stderr.write(f"FINAL: {self.final_text}\r\n")
            sys.stderr.flush()
        input_text(self.final_text, self.args.input_method)
        return 0

    def run_duration_or_ctrl_c(self) -> int:
        if not self.controller.start_recording():
            return 2

        sys.stderr.write(f"Recording from ALSA input {self.args.audio_device!r}. Press Ctrl+C to stop.\r\n")
        sys.stderr.flush()
        try:
            if self.args.duration > 0:
                time.sleep(self.args.duration)
            else:
                while True:
                    time.sleep(3600)
        except KeyboardInterrupt:
            pass
        finally:
            self.controller.stop_recording()
        return self._finish_and_input()

    def run_hold_key(self) -> int:
        key_code = parse_key_code(self.args.hold_key)
        backend = self.args.key_backend if self.args.key_backend != "auto" else None
        listener = create_key_listener(key_code, self.args.input_device, backend)
        recording = False
        backend_name = type(listener).__name__.replace("KeyListener", "").lower()
        sys.stderr.write(
            f"Hold {self.args.hold_key} (code {key_code}) to record; release to finish. "
            f"Backend: {backend_name}. Press Ctrl+C to exit.\r\n"
        )
        sys.stderr.flush()

        def on_press():
            nonlocal recording
            if not recording:
                if not self.controller.start_recording():
                    return
                recording = True

        def on_release():
            nonlocal recording
            if recording:
                ok = self.controller.stop_recording()
                recording = False
                if ok:
                    self._finish_and_input()
                self._reset_controller()

        try:
            listener.run(on_press, on_release)
        except KeyboardInterrupt:
            if recording:
                self.controller.stop_recording()
                return self._finish_and_input()
            return 0
        return 0

    def run(self) -> int:
        try:
            if self.args.hold_key:
                return self.run_hold_key()
            return self.run_duration_or_ctrl_c()
        finally:
            self.controller.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=default_socket_path())
    parser.add_argument("--segment-seconds", type=int, default=5, help="Live segment duration sent to backend")
    parser.add_argument("--audio-device", default="default", help="ALSA capture device for arecord -D (default: default; examples: hw:1,0, plughw:1,0)")
    parser.add_argument("--list-audio-devices", action="store_true", help="List ALSA capture devices and exit")
    parser.add_argument("--duration", type=float, default=0, help="Record this many seconds; 0 means until Ctrl+C")
    parser.add_argument("--hold-key", help="Evdev key name/code: press to start one recognition, release to finish and output (e.g. F12, KEY_F12, 88)")
    parser.add_argument("--input-device", help="Specific /dev/input/eventX device for evdev backend")
    parser.add_argument("--key-backend", choices=["auto", "terminal", "evdev"], default="auto",
                        help="Key listener backend: terminal (raw stdin, no special perms), evdev (needs input group), auto (try terminal first)")
    parser.add_argument("--final-timeout", type=float, default=180)
    parser.add_argument(
        "--input-method",
        choices=["auto", "clipboard", "stdout"],
        default="auto",
        help="How to deliver final text: auto/clipboard copies to clipboard; stdout prints to stdout",
    )
    parser.add_argument("--print-final", action="store_true", help="Also print final text to stderr")
    parser.add_argument("--quiet", action="store_true")
    # Config utility actions (test/diagnostic use only; not a production config UI)
    parser.add_argument("--reload-config", action="store_true",
                        help="Ask the backend to reload text-processing.json and exit")
    parser.add_argument("--config-status", action="store_true",
                        help="Print backend config status as JSON and exit")
    args = parser.parse_args()

    if args.list_audio_devices:
        return list_audio_devices()

    # ── Config utility actions ──────────────────────────────────────────
    if args.reload_config or args.config_status:
        try:
            client = JsonRpcClient(args.socket)
            client.connect()
        except (FileNotFoundError, ConnectionRefusedError, OSError) as exc:
            print(f"ERROR: cannot connect to backend at {args.socket}: {exc}", file=sys.stderr)
            return 1
        try:
            if args.config_status:
                result = client.call("vibetype.configStatus", {}, timeout=10.0)
                print(json.dumps(result, ensure_ascii=False, indent=2))
            if args.reload_config:
                result = client.call("vibetype.reloadConfig", {}, timeout=10.0)
                print(json.dumps(result, ensure_ascii=False, indent=2))
        except (RuntimeError, TimeoutError) as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
        finally:
            client.close()
        return 0

    if args.segment_seconds <= 0:
        parser.error("--segment-seconds must be > 0")

    try:
        return CliApp(args).run()
    except ConnectionRefusedError:
        print(
            f"ERROR: Cannot connect to vibetype-backend at {args.socket}.\n"
            f"       Start the backend first: vibetype-backend -s {args.socket}",
            file=sys.stderr,
        )
        return 1
    except FileNotFoundError:
        print(
            f"ERROR: Socket not found at {args.socket}.\n"
            f"       Is vibetype-backend running?",
            file=sys.stderr,
        )
        return 1
    except KeyboardInterrupt:
        return 0
    except Exception as exc:
        print(f"ERROR: unexpected error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
