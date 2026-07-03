#!/usr/bin/env python3
"""Vibetype Fcitx5 helper — subprocess launched by the fcitx5 C++ addon.

Communicates with the parent (vibetype.so) via stdin/stdout:

stdin (commands):
  record                            start recording
  stop                              stop recording & wait for final
  status                            query model status
  config.key=val\n...               set config key=val pairs
  quit                              exit

stdout (events):
  ready                             helper initialized, model checked
  status:TEXT                       auxiliary status update
  commit:TEXT                       final text to commit
  error:MSG                         error message
"""

from __future__ import annotations

import json
import os
import sys
import threading
import uuid
from pathlib import Path

SOURCE_DIR = Path(__file__).resolve().parent.parent / "common"
for p in (SOURCE_DIR, Path("/usr/share/vibetype/python")):
    if p.exists():
        sys.path.insert(0, str(p))

from vibetype_client import (  # noqa: E402
    VibetypeController,
    default_socket_path,
    ensure_runtime_session_dir,
)


class Fcitx5Helper:
    """Reads commands from stdin, drives VibetypeController, writes to stdout."""

    def __init__(self):
        self.socket_path = default_socket_path()
        self.segment_seconds = 20
        self.audio_device = "default"
        self.controller: VibetypeController | None = None
        self._lock = threading.Lock()
        self._recording = False
        self._model_ready = False
        self._indicator_stop = threading.Event()
        self._indicator_thread: threading.Thread | None = None
        self._indicator_label = "Recording"
        self._indicator_frames = ["●○○", "○●○", "○○●"]
        # Flush stdout immediately so the C++ parent gets lines in real time
        sys.stdout.reconfigure(line_buffering=True)
        sys.stderr.reconfigure(line_buffering=True)

    def log(self, msg: str) -> None:
        print(msg, file=sys.stderr, flush=True)

    def emit(self, event: str) -> None:
        print(event, flush=True)

    # ── config ────────────────────────────────────────────────────────

    def apply_config(self, line: str) -> None:
        """Parse 'config.key=value' lines."""
        if not line.startswith("config."):
            return
        rest = line[len("config."):]
        eq = rest.find("=")
        if eq < 0:
            return
        key = rest[:eq]
        val = rest[eq + 1:]
        if key == "socket_path":
            self.socket_path = val
            self.log(f"  socket_path={val}")
        elif key == "segment_seconds":
            self.segment_seconds = int(val)
            self.log(f"  segment_seconds={val}")
        elif key == "audio_device":
            self.audio_device = val
            self.log(f"  audio_device={val}")

        if not self._recording:
            self._reset_controller()

    # ── controller lifecycle ──────────────────────────────────────────

    def _reset_controller(self) -> None:
        if self.controller is None:
            return
        try:
            self.controller.close()
        except Exception:
            pass
        self.controller = None
        self._model_ready = False

    def ensure_controller(self) -> VibetypeController:
        if self.controller is not None:
            return self.controller
        self.controller = VibetypeController(
            socket_path=self.socket_path,
            segment_seconds=self.segment_seconds,
            commit_callback=self._on_commit,
            status_callback=self._on_status,
            audio_device=self.audio_device,
            client_name="fcitx5-helper",
            frontend_name="fcitx5",
        )
        return self.controller

    def check_model(self) -> None:
        try:
            ctl = self.ensure_controller()
            ctl.connect()
            status = ctl.model_status()
            state = status.get("state", "unknown")
            msg = status.get("message", "")
            self.log(f"Model status: {state} — {msg}")
            if state == "ready":
                self._model_ready = True
                self.emit("ready")
            else:
                self._model_ready = False
                self.emit(f"status:model {state}: {msg}")
        except Exception as exc:
            self.log(f"Model check failed: {exc}")
            self.emit(f"error:{exc}")

    # ── recording indicator ───────────────────────────────────────────

    def _indicator_text(self, frame_index: int) -> str:
        with self._lock:
            label = self._indicator_label.strip() or "Recording"
        return f"🎙 {label} {self._indicator_frames[frame_index % len(self._indicator_frames)]}"

    def _indicator_loop(self) -> None:
        frame = 0
        while not self._indicator_stop.wait(0.35):
            self.emit(f"status:{self._indicator_text(frame)}")
            frame += 1

    def _start_indicator(self, label: str = "Recording") -> None:
        self._stop_indicator()
        with self._lock:
            self._indicator_label = label
        self._indicator_stop.clear()
        self.emit(f"status:{self._indicator_text(0)}")
        self._indicator_thread = threading.Thread(
            target=self._indicator_loop, name="vibetype-fcitx5-indicator", daemon=True
        )
        self._indicator_thread.start()

    def _update_indicator(self, label: str) -> None:
        with self._lock:
            self._indicator_label = label.strip() or "Recording"

    def _stop_indicator(self) -> None:
        self._indicator_stop.set()
        if self._indicator_thread and self._indicator_thread.is_alive():
            self._indicator_thread.join(timeout=1)
        self._indicator_thread = None

    # ── recording ─────────────────────────────────────────────────────

    def start_recording(self) -> None:
        with self._lock:
            if self._recording:
                self.log("Already recording, ignoring")
                return
            self._recording = True

        try:
            ctl = self.ensure_controller()
            ok = ctl.start_recording()
            if ok:
                self.emit("recording-on")
                self._start_indicator()
            else:
                with self._lock:
                    self._recording = False
                self.emit("status:Vibetype start failed — check model")
        except Exception as exc:
            with self._lock:
                self._recording = False
            self.log(f"Start failed: {exc}")
            self.emit(f"error:{exc}")

    def stop_recording(self) -> None:
        with self._lock:
            if not self._recording:
                self.log("Not recording, ignoring stop")
                return
            self._recording = False

        try:
            self._stop_indicator()
            self.emit("status:⏳ Processing...")
            ctl = self.ensure_controller()
            ctl.stop_recording()
            self.emit("recording-off")
            # _on_commit will emit commit:TEXT when finalResult arrives
        except Exception as exc:
            self.log(f"Stop failed: {exc}")
            self.emit(f"error:{exc}")

    # ── callbacks (called from VibetypeController's RPC reader thread) ─

    def _on_commit(self, text: str) -> None:
        self._stop_indicator()
        self.emit(f"commit:{text}")
        self.emit("status:Vibetype done")

    def _on_status(self, text: str) -> None:
        if text == "recording":
            self._update_indicator("Recording")
            return
        if text.startswith("partial: "):
            partial = text[len("partial: ") :].strip()
            self._update_indicator(partial or "Recording")
            return
        if text in {"stopping", "waiting final result"}:
            self._stop_indicator()
            self.emit("status:⏳ Processing...")
            return
        if text in {"paused", "final result ready"}:
            self._stop_indicator()
            return
        self.emit(f"status:{text}")

    # ── main loop ─────────────────────────────────────────────────────

    def run(self) -> None:
        self.log("Vibetype Fcitx5 helper started")
        self.check_model()

        for raw in sys.stdin:
            line = raw.strip()
            if not line:
                continue

            self.log(f"  <- {line}")

            if line == "quit":
                break
            elif line == "record":
                self.start_recording()
            elif line == "stop":
                self.stop_recording()
            elif line == "status":
                self.check_model()
            elif line.startswith("config."):
                self.apply_config(line)
            else:
                self.log(f"Unknown command: {line}")

        self.log("Vibetype Fcitx5 helper exiting")
        self._stop_indicator()
        if self.controller:
            try:
                if self._recording:
                    self.controller.stop_recording()
                self.controller.close()
            except Exception:
                pass


def main() -> int:
    helper = Fcitx5Helper()
    try:
        helper.run()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
