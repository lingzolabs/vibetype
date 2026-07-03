#!/usr/bin/env python3
"""Vibetype IBus frontend and standalone protocol test client."""

from __future__ import annotations

import argparse
import json
import sys
import uuid
from pathlib import Path

SOURCE_COMMON_DIR = Path(__file__).resolve().parents[1] / "common"
INSTALLED_COMMON_DIR = Path(__file__).resolve().parents[1] / "share" / "vibetype" / "python"
for path in (SOURCE_COMMON_DIR, INSTALLED_COMMON_DIR, Path("/usr/share/vibetype/python")):
    if path.exists():
        sys.path.insert(0, str(path))

from vibetype_client import (  # noqa: E402
    VibetypeController,
    default_socket_path,
    ensure_runtime_session_dir,
    record_default_alsa_to_wav,
    wav_stats,
    write_silence_wav,
)


def run_ibus(socket_path: str, segment_seconds: int, trigger_key: str) -> int:
    import gi
    import threading

    gi.require_version("IBus", "1.0")
    from gi.repository import GLib, GObject, IBus

    IBus.init()

    trigger_keyval = getattr(IBus, f"KEY_{trigger_key}", IBus.KEY_F12)

    class VibetypeEngine(IBus.Engine):
        __gtype_name__ = "VibetypeEngine"

        def __init__(self, engine_name: str, object_path: str, connection):
            super().__init__(engine_name=engine_name, object_path=object_path, connection=connection)
            self.controller = VibetypeController(
                socket_path,
                segment_seconds,
                commit_callback=lambda text: GLib.idle_add(self._commit_text, text),
                status_callback=lambda text: GLib.idle_add(self._show_status, text),
            )

        def do_process_key_event(self, keyval, keycode, state):
            if keyval == trigger_keyval and not (state & IBus.ModifierType.RELEASE_MASK):
                threading.Thread(target=self.controller.toggle_recording, daemon=True).start()
                return True
            return False

        def _commit_text(self, text: str):
            self.commit_text(IBus.Text.new_from_string(text))
            return False

        def _show_status(self, text: str):
            self.update_auxiliary_text(IBus.Text.new_from_string(text), True)
            return False

    GObject.type_register(VibetypeEngine)

    bus = IBus.Bus()
    factory = IBus.Factory.new(bus.get_connection())
    factory.add_engine("vibetype", VibetypeEngine)
    bus.request_name("org.freedesktop.IBus.Vibetype", 0)
    GLib.MainLoop().run()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=default_socket_path())
    parser.add_argument("--segment-seconds", type=int, default=20)
    parser.add_argument("--trigger-key", default="F12")
    parser.add_argument("--ibus", action="store_true", help="Run as an IBus engine process")
    parser.add_argument("--test-wav", help="Submit a WAV file without IBus integration")
    parser.add_argument("--test-silence-ms", type=int, help="Generate and submit a silent WAV")
    parser.add_argument("--test-record", type=int, metavar="SECONDS", help="Record once from ALSA default input, then submit")
    parser.add_argument("--record-only", type=int, metavar="SECONDS", help="Record from ALSA default input and print WAV stats without backend")
    parser.add_argument("--record-out", help="Output WAV path for --record-only")
    parser.add_argument("--no-wait-model", action="store_true")
    args = parser.parse_args()

    if args.ibus:
        return run_ibus(args.socket, args.segment_seconds, args.trigger_key)

    controller = VibetypeController(args.socket, args.segment_seconds, commit_callback=lambda _text: None)
    if args.record_only:
        session_id = str(uuid.uuid4())
        session_dir = ensure_runtime_session_dir(session_id)
        wav = Path(args.record_out) if args.record_out else session_dir / "record-only.wav"
        record_default_alsa_to_wav(wav, args.record_only)
        print("RECORDED:", wav)
        print("STATS:", json.dumps(wav_stats(wav), ensure_ascii=False))
        return 0

    if args.test_wav:
        text = controller.submit_wav_for_test(Path(args.test_wav), wait_model=not args.no_wait_model)
        print("COMMIT:", text)
        return 0
    if args.test_silence_ms is not None:
        session_id = str(uuid.uuid4())
        session_dir = ensure_runtime_session_dir(session_id)
        wav = session_dir / "silence.wav"
        write_silence_wav(wav, args.test_silence_ms)
        text = controller.submit_wav_for_test(wav, wait_model=not args.no_wait_model)
        print("COMMIT:", text)
        return 0
    if args.test_record:
        text = controller.test_record_once(args.test_record)
        print("COMMIT:", text)
        return 0

    parser.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
