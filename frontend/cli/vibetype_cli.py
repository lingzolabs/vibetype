#!/usr/bin/env python3
"""Realtime sound-card CLI for Vibetype.

Records from ALSA default input, sends live WAV segments to vibetype-backend,
waits for the final result, then inputs the text with a selected commit method.
"""

from __future__ import annotations

import argparse
import glob
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
        raise RuntimeError("no readable input event devices. Try --input-device /dev/input/eventX or run with input-group permissions. " + "; ".join(errors[:3]))
    return files


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


def paste_from_clipboard() -> bool:
    # Clipboard carries the multilingual text; these helpers only press Ctrl+V.
    paste_commands = []
    if command_exists("xdotool"):
        paste_commands.append(["xdotool", "key", "--clearmodifiers", "ctrl+v"])
    if command_exists("ydotool"):
        paste_commands.append(["ydotool", "key", "29:1", "47:1", "47:0", "29:0"])

    for cmd in paste_commands:
        try:
            subprocess.check_call(cmd)
            return True
        except (FileNotFoundError, subprocess.CalledProcessError) as exc:
            print(f"WARN: paste command failed: {' '.join(cmd)}: {exc}", file=sys.stderr)
    return False


def input_text(text: str, method: str) -> None:
    if not text:
        return

    if method in ("auto", "paste", "clipboard"):
        if copy_to_clipboard(text):
            if method in ("auto", "paste"):
                time.sleep(0.05)
                if paste_from_clipboard():
                    print("Pasted recognized text from clipboard.", file=sys.stderr)
                    return
                print("Copied recognized text to clipboard, but auto-paste is unavailable. Paste it with Ctrl+V.", file=sys.stderr)
                return
            print("Copied recognized text to clipboard. Paste it with Ctrl+V.", file=sys.stderr)
            return
        if method in ("paste", "clipboard"):
            print("WARN: no working clipboard tool found; falling back to stdout", file=sys.stderr)

    if method == "stdout" or method in ("auto", "paste", "clipboard"):
        print(text)
        return

    raise ValueError(f"unknown input method: {method}")


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
        )

    def _reset_controller(self) -> None:
        # Keep the JSON-RPC connection alive across press/release sessions.
        # Only reset per-utterance state; close the socket only when the CLI exits.
        self.final_text = ""

    def _on_status(self, text: str) -> None:
        if not self.args.quiet:
            print(f"STATUS: {text}", file=sys.stderr, flush=True)

    def _on_commit(self, text: str) -> None:
        self.final_text = text

    def _finish_and_input(self) -> int:
        if not self.controller.final_event.wait(timeout=self.args.final_timeout):
            print("ERROR: timed out waiting for finalResult", file=sys.stderr)
            return 1
        if self.args.print_final:
            print(f"FINAL: {self.final_text}", file=sys.stderr)
        input_text(self.final_text, self.args.input_method)
        return 0

    def run_duration_or_ctrl_c(self) -> int:
        if not self.controller.start_recording():
            return 2

        print(f"Recording from ALSA input {self.args.audio_device!r}. Press Ctrl+C to stop.", file=sys.stderr)
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
        event_struct = struct.Struct("llHHi")
        files = open_input_devices(self.args.input_device)
        recording = False
        print(
            f"Hold {self.args.hold_key} (evdev code {key_code}) to record one utterance; release to finish and output. Press Ctrl+C to exit.",
            file=sys.stderr,
            flush=True,
        )
        try:
            while True:
                readable, _, _ = select.select(files, [], [])
                for f in readable:
                    data = os.read(f.fileno(), event_struct.size)
                    if len(data) != event_struct.size:
                        continue
                    _sec, _usec, ev_type, code, value = event_struct.unpack(data)
                    if ev_type != EV_KEY or code != key_code:
                        continue
                    if value == 1 and not recording:
                        # Each press starts a new independent recognition session.
                        if not self.controller.start_recording():
                            return 2
                        recording = True
                    elif value == 0 and recording:
                        # Release finalizes this session and outputs only this result.
                        self.controller.stop_recording()
                        recording = False
                        code = self._finish_and_input()
                        if code != 0:
                            return code
                        self._reset_controller()
        except KeyboardInterrupt:
            if recording:
                self.controller.stop_recording()
                return self._finish_and_input()
            return 0
        finally:
            for f in files:
                f.close()

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
    parser.add_argument("--input-device", help="Specific /dev/input/eventX device for --hold-key; default scans readable event devices")
    parser.add_argument("--final-timeout", type=float, default=180)
    parser.add_argument(
        "--input-method",
        choices=["auto", "paste", "clipboard", "stdout"],
        default="auto",
        help="How to deliver final text: auto/paste copies to clipboard and tries Ctrl+V; clipboard only copies; stdout is for debugging",
    )
    parser.add_argument("--print-final", action="store_true", help="Also print final text to stderr")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.list_audio_devices:
        return list_audio_devices()

    if args.segment_seconds <= 0:
        parser.error("--segment-seconds must be > 0")
    return CliApp(args).run()


if __name__ == "__main__":
    raise SystemExit(main())
