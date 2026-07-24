#!/usr/bin/env python3
"""Vibetype IBus frontend, setup dialog, and standalone protocol test client."""

from __future__ import annotations

import argparse
import configparser
import json
import os
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
from vibetype_config import (  # noqa: E402
    load_text_proc_config,
    patch_text_proc_config,
    get_enable_builtin_corrections,
    get_enable_qwen_polish,
    get_custom_corrections,
    corrections_to_text,
    corrections_from_text,
)


IBUS_CONFIG_SECTION = "ibus"
IBUS_DEFAULTS = {
    "TriggerKey": "F12",
    "SegmentSeconds": "20",
    "SocketPath": "",
    "AudioDevice": "default",
}


def ibus_config_path() -> Path:
    base = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
    return base / "vibetype" / "ibus.conf"


def load_ibus_settings() -> dict[str, str]:
    settings = dict(IBUS_DEFAULTS)
    config = configparser.ConfigParser()
    config.optionxform = str
    path = ibus_config_path()
    if not path.exists():
        return settings
    config.read(path, encoding="utf-8")
    if not config.has_section(IBUS_CONFIG_SECTION):
        return settings
    section = config[IBUS_CONFIG_SECTION]
    for key, default in IBUS_DEFAULTS.items():
        value = section.get(key, fallback=default)
        if value is not None:
            settings[key] = value
    return settings


def parse_segment_seconds(value: str | int) -> int:
    try:
        seconds = int(value)
    except (TypeError, ValueError):
        seconds = int(IBUS_DEFAULTS["SegmentSeconds"])
    return max(1, seconds)


def save_ibus_settings(settings: dict[str, str]) -> Path:
    normalized = {
        "TriggerKey": settings.get("TriggerKey", IBUS_DEFAULTS["TriggerKey"]).strip()
        or IBUS_DEFAULTS["TriggerKey"],
        "SegmentSeconds": str(parse_segment_seconds(settings.get("SegmentSeconds", "20"))),
        "SocketPath": settings.get("SocketPath", "").strip(),
        "AudioDevice": settings.get("AudioDevice", IBUS_DEFAULTS["AudioDevice"]).strip()
        or IBUS_DEFAULTS["AudioDevice"],
    }
    path = ibus_config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    config = configparser.ConfigParser()
    config.optionxform = str
    config[IBUS_CONFIG_SECTION] = normalized
    with path.open("w", encoding="utf-8") as fh:
        config.write(fh)
    return path


def trigger_state_mask(IBus) -> int:
    return int(
        IBus.ModifierType.SHIFT_MASK
        | IBus.ModifierType.CONTROL_MASK
        | IBus.ModifierType.MOD1_MASK
        | IBus.ModifierType.SUPER_MASK
        | IBus.ModifierType.HYPER_MASK
        | IBus.ModifierType.META_MASK
    )


def parse_trigger_key(trigger_key: str, IBus) -> tuple[int, int]:
    shortcut = (trigger_key or "").strip()
    if not shortcut:
        raise ValueError("trigger key must not be empty")
    ok, keyval, modifiers = IBus.key_event_from_string(shortcut)
    if not ok:
        raise ValueError(
            f"invalid trigger key {shortcut!r}; examples: F12, Control+F12, Alt+space"
        )
    return int(keyval), int(modifiers) & trigger_state_mask(IBus)


def run_setup() -> int:
    settings = load_ibus_settings()
    config_path = ibus_config_path()

    # Load shared text-processing config (do not apply defaults to file).
    try:
        tp_data = load_text_proc_config()
    except ValueError:
        tp_data = {}

    tp_enable_builtin = get_enable_builtin_corrections(tp_data)
    tp_enable_qwen = get_enable_qwen_polish(tp_data)
    tp_corrections = get_custom_corrections(tp_data)

    try:
        import gi

        gi.require_version("Gtk", "3.0")
        gi.require_version("IBus", "1.0")
        from gi.repository import Gtk, IBus
    except Exception as exc:
        save_ibus_settings(settings)
        print(f"IBus setup UI unavailable ({exc}). Edit {config_path} manually.")
        return 0

    IBus.init()

    window = Gtk.Window(title="Vibetype IBus Settings")
    window.set_border_width(12)
    window.set_resizable(False)

    outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
    window.add(outer)

    title = Gtk.Label()
    title.set_markup("<b>Vibetype IBus Settings</b>")
    title.set_xalign(0)
    outer.pack_start(title, False, False, 0)

    grid = Gtk.Grid(column_spacing=12, row_spacing=8)
    outer.pack_start(grid, False, False, 0)

    trigger_entry = Gtk.Entry(text=settings["TriggerKey"])
    segment_spin = Gtk.SpinButton.new_with_range(1, 600, 1)
    segment_spin.set_value(parse_segment_seconds(settings["SegmentSeconds"]))
    socket_entry = Gtk.Entry(text=settings["SocketPath"])
    audio_entry = Gtk.Entry(text=settings["AudioDevice"])

    rows = [
        ("Trigger key", trigger_entry),
        ("Segment seconds", segment_spin),
        ("Socket path", socket_entry),
        ("ALSA audio device", audio_entry),
    ]
    for index, (label_text, widget) in enumerate(rows):
        label = Gtk.Label(label=label_text)
        label.set_xalign(0)
        grid.attach(label, 0, index, 1, 1)
        widget.set_hexpand(True)
        grid.attach(widget, 1, index, 1, 1)

    hint = Gtk.Label(
        label=(
            "Examples: F12, Control+F12, Alt+space, Super+space. "
            "Leave socket path empty to use the default runtime socket."
        )
    )
    hint.set_xalign(0)
    hint.set_line_wrap(True)
    outer.pack_start(hint, False, False, 0)

    # ── Text-processing section ───────────────────────────────────────
    sep = Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL)
    outer.pack_start(sep, False, False, 0)

    tp_title = Gtk.Label()
    tp_title.set_markup("<b>Text Processing</b>")
    tp_title.set_xalign(0)
    outer.pack_start(tp_title, False, False, 0)

    tp_grid = Gtk.Grid(column_spacing=12, row_spacing=8)
    outer.pack_start(tp_grid, False, False, 0)

    builtin_check = Gtk.CheckButton(label="Enable built-in computer-term corrections")
    builtin_check.set_active(tp_enable_builtin)
    tp_grid.attach(builtin_check, 0, 0, 2, 1)

    qwen_check = Gtk.CheckButton(label="Enable Qwen LLM polish")
    qwen_check.set_active(tp_enable_qwen)
    tp_grid.attach(qwen_check, 0, 1, 2, 1)

    corr_label = Gtk.Label(label="Custom corrections (one per line, format: wrong=correct)")
    corr_label.set_xalign(0)
    tp_grid.attach(corr_label, 0, 2, 2, 1)

    corr_tv = Gtk.TextView()
    corr_tv.set_size_request(400, 100)
    corr_tv.set_accepts_tab(False)
    corr_buf = corr_tv.get_buffer()
    corr_buf.set_text(corrections_to_text(tp_corrections))

    corr_scroll = Gtk.ScrolledWindow()
    corr_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
    corr_scroll.add(corr_tv)
    tp_grid.attach(corr_scroll, 0, 3, 2, 1)

    tp_hint = Gtk.Label(
        label="Each line: wrong_word=correct_word  (first '=' separates key from value)"
    )
    tp_hint.set_xalign(0)
    tp_hint.set_line_wrap(True)
    outer.pack_start(tp_hint, False, False, 0)

    # ── Buttons ──────────────────────────────────────────────────────────
    buttons = Gtk.ButtonBox(orientation=Gtk.Orientation.HORIZONTAL)
    buttons.set_layout(Gtk.ButtonBoxStyle.END)
    outer.pack_start(buttons, False, False, 0)

    cancel_button = Gtk.Button(label="Cancel")
    save_button = Gtk.Button(label="Save")
    buttons.add(cancel_button)
    buttons.add(save_button)

    def show_error(message: str) -> None:
        dialog = Gtk.MessageDialog(
            transient_for=window,
            flags=0,
            message_type=Gtk.MessageType.ERROR,
            buttons=Gtk.ButtonsType.OK,
            text="Invalid Vibetype IBus settings",
        )
        dialog.format_secondary_text(message)
        dialog.run()
        dialog.destroy()

    def on_save(_button) -> None:
        trigger_key = trigger_entry.get_text().strip() or IBUS_DEFAULTS["TriggerKey"]
        try:
            parse_trigger_key(trigger_key, IBus)
        except ValueError as exc:
            show_error(str(exc))
            return

        # Validate and save the shared config first so an error does not leave
        # the two configuration files partially updated.
        start_iter, end_iter = corr_buf.get_bounds()
        corr_text = corr_buf.get_text(start_iter, end_iter, False)
        new_corrections = corrections_from_text(corr_text)
        try:
            patch_text_proc_config(
                {
                    "enable_builtin_corrections": builtin_check.get_active(),
                    "enable_qwen_polish": qwen_check.get_active(),
                    "custom_corrections": new_corrections,
                }
            )
        except (ValueError, OSError) as exc:
            show_error(f"Failed to save text-processing config: {exc}")
            return

        save_ibus_settings(
            {
                "TriggerKey": trigger_key,
                "SegmentSeconds": str(segment_spin.get_value_as_int()),
                "SocketPath": socket_entry.get_text().strip(),
                "AudioDevice": audio_entry.get_text().strip(),
            }
        )

        # Optionally notify running backend to reload (best-effort, non-blocking).
        socket_path = socket_entry.get_text().strip() or default_socket_path()
        _try_reload_config(socket_path)

        window.destroy()

    cancel_button.connect("clicked", lambda _button: window.destroy())
    save_button.connect("clicked", on_save)
    window.connect("destroy", lambda *_args: Gtk.main_quit())
    window.show_all()
    Gtk.main()
    return 0


def _try_reload_config(socket_path: str) -> None:
    """Best-effort reload after an atomic save; the next session is fallback."""
    try:
        from vibetype_client import JsonRpcClient
        client = JsonRpcClient(socket_path)
        client.connect()
        try:
            client.call("vibetype.reloadConfig", {}, timeout=2.0)
        finally:
            client.close()
    except Exception:
        pass


def run_ibus(socket_path: str, segment_seconds: int, trigger_key: str, audio_device: str) -> int:
    import gi
    import threading

    gi.require_version("IBus", "1.0")
    from gi.repository import GLib, GObject, IBus

    IBus.init()

    try:
        trigger_keyval, trigger_modifiers = parse_trigger_key(trigger_key, IBus)
    except ValueError as exc:
        fallback = IBUS_DEFAULTS["TriggerKey"]
        sys.stderr.write(f"Vibetype IBus: {exc}; falling back to {fallback}.\n")
        trigger_keyval, trigger_modifiers = parse_trigger_key(fallback, IBus)

    class VibetypeEngine(IBus.Engine):
        __gtype_name__ = "VibetypeEngine"

        def __init__(self, **kwargs):
            super().__init__(**kwargs)
            self._init_state()

        def _init_state(self):
            """Initialize instance state; safe to call multiple times."""
            if hasattr(self, "_initialized"):
                return
            self._initialized = True
            self._controller = None
            self._recording = False
            self._busy = False
            self._indicator_source_id = 0
            self._indicator_frame = 0
            self._indicator_frames = ["🎤 ●○○", "🎤 ○●○", "🎤 ○○●"]

        def _ensure_state(self):
            """Guard for GObject construction that may skip __init__."""
            if not hasattr(self, "_initialized"):
                self._init_state()

        def _get_controller(self):
            """Lazily create the controller on first use."""
            self._ensure_state()
            if self._controller is None:
                self._controller = VibetypeController(
                    socket_path,
                    segment_seconds,
                    commit_callback=lambda text: GLib.idle_add(self._commit_text, text),
                    status_callback=lambda text: GLib.idle_add(self._show_status, text),
                    audio_device=audio_device,
                    client_name="ibus-python",
                    frontend_name="ibus",
                )
            return self._controller

        def _do_start(self):
            """Run in a worker thread: connect and start recording."""
            try:
                controller = self._get_controller()
                controller.start_recording()
                GLib.idle_add(self._set_recording_state, True)
            except Exception as exc:
                GLib.idle_add(self._show_status, f"error: {exc}")
            finally:
                self._busy = False

        def _do_stop(self):
            """Run in a worker thread: stop recording and wait for result."""
            try:
                controller = self._get_controller()
                controller.stop_recording()
            except Exception as exc:
                GLib.idle_add(self._show_status, f"error: {exc}")
            finally:
                self._busy = False
                GLib.idle_add(self._set_recording_state, False)

        def _set_recording_state(self, on):
            self._recording = on
            return False

        def do_process_key_event(self, keyval, keycode, state):
            self._ensure_state()
            # Fast path: only handle the configured trigger key
            if int(keyval) != trigger_keyval:
                return False
            # Check modifiers match (mask out release flag)
            active_mods = int(state) & trigger_state_mask(IBus)
            if active_mods != trigger_modifiers:
                return False

            is_release = bool(state & IBus.ModifierType.RELEASE_MASK)

            # Ignore key-repeat while already recording/busy
            if not is_release and self._recording:
                return True
            if is_release and not self._recording:
                return True
            if self._busy:
                return True

            self._busy = True
            if is_release:
                threading.Thread(target=self._do_stop, daemon=True).start()
            else:
                threading.Thread(target=self._do_start, daemon=True).start()
            return True

        def do_disable(self):
            """Called when the engine is disabled; clean up."""
            self._ensure_state()
            if self._recording and self._controller:
                self._recording = False
                threading.Thread(target=self._safe_stop, daemon=True).start()
            self._stop_indicator(clear=True)

        def _safe_stop(self):
            try:
                if self._controller:
                    self._controller.stop_recording()
            except Exception:
                pass

        def _set_indicator(self, text: str) -> None:
            if not text:
                self.hide_auxiliary_text()
                self.hide_preedit_text()
                return
            ibus_text = IBus.Text.new_from_string(text)
            self.update_auxiliary_text(ibus_text, True)
            self.show_auxiliary_text()

        def _render_recording_indicator(self) -> None:
            frame = self._indicator_frames[self._indicator_frame % len(self._indicator_frames)]
            self._set_indicator(frame)
            self._indicator_frame += 1

        def _tick_indicator(self):
            self._render_recording_indicator()
            return True

        def _start_indicator(self) -> None:
            self._indicator_frame = 0
            if not self._indicator_source_id:
                self._render_recording_indicator()
                self._indicator_source_id = GLib.timeout_add(350, self._tick_indicator)

        def _stop_indicator(self, clear: bool = False) -> None:
            if self._indicator_source_id:
                GLib.source_remove(self._indicator_source_id)
                self._indicator_source_id = 0
            if clear:
                self._set_indicator("")

        def _commit_text(self, text: str):
            self._stop_indicator(clear=True)
            self.commit_text(IBus.Text.new_from_string(text))
            return False

        def _show_status(self, text: str):
            if text == "recording":
                self._start_indicator()
                return False
            if text.startswith("partial: "):
                # Partial results: keep the recording animation running
                return False
            if text in {"stopping", "waiting final result"}:
                self._stop_indicator()
                self._set_indicator("⏳ Processing...")
                return False
            if text in {"paused", "final result ready"}:
                self._stop_indicator(clear=True)
                return False
            if text.startswith(("error:", "cancelled:")):
                # Error or cancellation: stop animation and clear the indicator
                # after a brief pause so the user can see the message.
                self._stop_indicator()
                if text.startswith("cancelled:"):
                    self._set_indicator("")
                else:
                    self._set_indicator(text)
                return False
            self._stop_indicator()
            self._set_indicator(text)
            return False

    GObject.type_register(VibetypeEngine)

    bus = IBus.Bus()
    factory = IBus.Factory.new(bus.get_connection())
    factory.add_engine("vibetype", VibetypeEngine)
    bus.request_name("org.freedesktop.IBus.Vibetype", 0)
    GLib.MainLoop().run()
    return 0


def main() -> int:
    settings = load_ibus_settings()

    parser = argparse.ArgumentParser()
    parser.add_argument("--socket", default=settings["SocketPath"] or default_socket_path())
    parser.add_argument("--segment-seconds", type=int, default=parse_segment_seconds(settings["SegmentSeconds"]))
    parser.add_argument("--trigger-key", default=settings["TriggerKey"])
    parser.add_argument("--audio-device", default=settings["AudioDevice"] or IBUS_DEFAULTS["AudioDevice"])
    parser.add_argument("--setup", action="store_true", help="Open the IBus settings editor")
    parser.add_argument("--ibus", action="store_true", help="Run as an IBus engine process")
    parser.add_argument("--test-wav", help="Submit a WAV file without IBus integration")
    parser.add_argument("--test-silence-ms", type=int, help="Generate and submit a silent WAV")
    parser.add_argument("--test-record", type=int, metavar="SECONDS", help="Record once from ALSA default input, then submit")
    parser.add_argument("--record-only", type=int, metavar="SECONDS", help="Record from ALSA default input and print WAV stats without backend")
    parser.add_argument("--record-out", help="Output WAV path for --record-only")
    parser.add_argument("--no-wait-model", action="store_true")
    args = parser.parse_args()

    if args.setup:
        return run_setup()

    if args.segment_seconds <= 0:
        parser.error("--segment-seconds must be > 0")

    if args.ibus:
        return run_ibus(args.socket, args.segment_seconds, args.trigger_key, args.audio_device)

    controller = VibetypeController(
        args.socket,
        args.segment_seconds,
        commit_callback=lambda _text: None,
        audio_device=args.audio_device,
        client_name="ibus-python-test",
        frontend_name="ibus",
    )
    if args.record_only:
        session_id = str(uuid.uuid4())
        session_dir = ensure_runtime_session_dir(session_id)
        wav = Path(args.record_out) if args.record_out else session_dir / "record-only.wav"
        record_default_alsa_to_wav(wav, args.record_only, args.audio_device)
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
