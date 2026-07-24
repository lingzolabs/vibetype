"""Shared Vibetype frontend client, audio capture, and JSON-RPC helpers."""


from __future__ import annotations

import array
import json
import math
import os
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import wave
from pathlib import Path
from typing import Callable, Optional


AUDIO_FORMAT = {
    "sample_rate": 16000,
    "channels": 1,
    "sample_format": "pcm_s16le",
    "container": "wav",
}


def runtime_dir() -> Path:
    return Path(os.environ.get("XDG_RUNTIME_DIR", tempfile.gettempdir()))


def default_socket_path() -> str:
    return str(runtime_dir() / "vibetype" / "vibetype.sock")


def ensure_runtime_session_dir(session_id: str) -> Path:
    path = runtime_dir() / "vibetype" / session_id
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_pcm16_wav(path: Path, pcm: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if len(pcm) % 2:
        pcm = pcm[:-1]
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(16000)
        wav.writeframes(pcm)


def write_silence_wav(path: Path, duration_ms: int) -> None:
    frames = max(1, 16000 * duration_ms // 1000)
    write_pcm16_wav(path, b"\x00\x00" * frames)


def wav_stats(path: Path) -> dict:
    with wave.open(str(path), "rb") as wav:
        frames = wav.readframes(wav.getnframes())
        stats = {
            "sample_rate": wav.getframerate(),
            "channels": wav.getnchannels(),
            "sample_width": wav.getsampwidth(),
            "frames": wav.getnframes(),
            "duration_ms": int(wav.getnframes() * 1000 / max(1, wav.getframerate())),
        }
    samples = array.array("h")
    samples.frombytes(frames)
    if sys.byteorder != "little":
        samples.byteswap()
    if samples:
        peak = max(abs(v) for v in samples)
        rms = math.sqrt(sum(v * v for v in samples) / len(samples))
    else:
        peak = 0
        rms = 0.0
    stats["peak"] = int(peak)
    stats["rms"] = round(rms, 2)
    return stats


class JsonRpcClient:
    def __init__(self, socket_path: str):
        self.socket_path = socket_path
        self.sock: Optional[socket.socket] = None
        self.file = None
        self.next_id = 1
        self.pending: dict[int, queue.Queue] = {}
        self.pending_lock = threading.Lock()
        self.notify_handlers: list[Callable[[str, dict], None]] = []
        self.reader: Optional[threading.Thread] = None
        self.running = False
        self.send_lock = threading.Lock()

    def connect(self) -> None:
        if self.running:
            return
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(self.socket_path)
        self.file = self.sock.makefile("rwb", buffering=0)
        self.running = True
        self.reader = threading.Thread(target=self._read_loop, name="vibetype-rpc", daemon=True)
        self.reader.start()

    def close(self) -> None:
        self.running = False
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        try:
            if self.file:
                self.file.close()
        finally:
            if self.sock:
                self.sock.close()

    def add_notify_handler(self, handler: Callable[[str, dict], None]) -> None:
        self.notify_handlers.append(handler)

    def call(self, method: str, params: Optional[dict] = None, timeout: float = 30.0) -> dict:
        if not self.running or not self.file:
            self.connect()
        req_id = self.next_id
        self.next_id += 1
        q: queue.Queue = queue.Queue(maxsize=1)
        with self.pending_lock:
            self.pending[req_id] = q
        self._send({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params or {}})
        try:
            msg = q.get(timeout=timeout)
        except queue.Empty as exc:
            with self.pending_lock:
                self.pending.pop(req_id, None)
            raise TimeoutError(f"JSON-RPC timeout: {method}") from exc
        if "error" in msg:
            raise RuntimeError(f"JSON-RPC error from {method}: {msg['error']}")
        return msg.get("result")

    def _send(self, msg: dict) -> None:
        data = (json.dumps(msg, ensure_ascii=False) + "\n").encode("utf-8")
        with self.send_lock:
            assert self.file is not None
            self.file.write(data)

    def _read_loop(self) -> None:
        assert self.file is not None
        while self.running:
            try:
                line = self.file.readline()
            except OSError:
                break
            if not line:
                break
            try:
                msg = json.loads(line.decode("utf-8"))
            except Exception:
                continue
            if "method" in msg and "id" not in msg:
                method = msg.get("method", "")
                params = msg.get("params") or {}
                for handler in list(self.notify_handlers):
                    handler(method, params)
                continue
            msg_id = msg.get("id")
            if isinstance(msg_id, int):
                with self.pending_lock:
                    q = self.pending.pop(msg_id, None)
                if q is not None:
                    q.put(msg)
        self.running = False


class ArecordSegmentRecorder:
    """Read PCM samples from ALSA input and write WAV segments.

    This uses `arecord` only as the ALSA capture backend. Python reads raw PCM
    bytes from stdout, owns segmentation, and writes the WAV files sent to the
    backend.
    """

    def __init__(self, segment_seconds: int, on_segment: Callable[[Path, int, int], None], audio_device: str = "default"):
        self.segment_seconds = segment_seconds
        self.on_segment = on_segment
        self.audio_device = audio_device or "default"
        self.stop_event = threading.Event()
        self.thread: Optional[threading.Thread] = None
        self.process: Optional[subprocess.Popen] = None
        self.session_dir: Optional[Path] = None
        self.index = 0

    def start(self, session_dir: Path, start_index: int = 0) -> None:
        self.session_dir = session_dir
        self.index = start_index
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._loop, name="vibetype-arecord", daemon=True)
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
        if self.thread:
            self.thread.join()

    def _open_arecord(self) -> subprocess.Popen:
        cmd = [
            "arecord",
            "-q",
            "-D",
            self.audio_device,
            "-f",
            "S16_LE",
            "-r",
            "16000",
            "-c",
            "1",
            "-t",
            "raw",
        ]
        return subprocess.Popen(cmd, stdout=subprocess.PIPE)

    def _emit_segment(self, pcm: bytearray, started: float) -> None:
        if not pcm:
            return
        assert self.session_dir is not None
        path = self.session_dir / f"segment-{self.index:03d}.wav"
        write_pcm16_wav(path, bytes(pcm))
        duration_ms = int((time.monotonic() - started) * 1000)
        self.on_segment(path, self.index, duration_ms)
        self.index += 1

    def _loop(self) -> None:
        assert self.session_dir is not None
        bytes_per_second = 16000 * 2
        segment_bytes = max(bytes_per_second, self.segment_seconds * bytes_per_second)
        chunk_bytes = 3200  # 100 ms of mono s16le at 16 kHz.
        pcm = bytearray()
        segment_started = time.monotonic()
        self.process = self._open_arecord()
        assert self.process.stdout is not None
        try:
            while not self.stop_event.is_set():
                chunk = self.process.stdout.read(chunk_bytes)
                if not chunk:
                    break
                pcm.extend(chunk)
                while len(pcm) >= segment_bytes:
                    segment = pcm[:segment_bytes]
                    del pcm[:segment_bytes]
                    self._emit_segment(segment, segment_started)
                    segment_started = time.monotonic()
            if pcm:
                self._emit_segment(pcm, segment_started)
        finally:
            if self.process and self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()


def copy_wav_to_runtime(src: Path, session_dir: Path, index: int) -> Path:
    dst = session_dir / f"segment-{index:03d}.wav"
    shutil.copyfile(src, dst)
    return dst


def record_default_alsa_to_wav(path: Path, duration_seconds: int, audio_device: str = "default") -> None:
    bytes_to_read = max(1, duration_seconds) * 16000 * 2
    cmd = [
        "arecord",
        "-q",
        "-D",
        audio_device or "default",
        "-f",
        "S16_LE",
        "-r",
        "16000",
        "-c",
        "1",
        "-t",
        "raw",
    ]
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    assert process.stdout is not None
    pcm = bytearray()
    try:
        while len(pcm) < bytes_to_read:
            chunk = process.stdout.read(min(3200, bytes_to_read - len(pcm)))
            if not chunk:
                break
            pcm.extend(chunk)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
    if len(pcm) < 1600:
        raise RuntimeError("ALSA capture produced too little audio data")
    write_pcm16_wav(path, bytes(pcm))


class VibetypeError(RuntimeError):
    """Custom exception for Vibetype frontend errors with a user-facing message."""
    pass


class VibetypeController:
    def __init__(
        self,
        socket_path: str,
        segment_seconds: int = 20,
        commit_callback: Optional[Callable[[str], None]] = None,
        status_callback: Optional[Callable[[str], None]] = None,
        audio_device: str = "default",
        client_name: str = "ibus-python",
        frontend_name: str = "ibus-python",
        min_record_ms: int = 1000,
    ):
        self.client = JsonRpcClient(socket_path)
        self.client.add_notify_handler(self._on_notify)
        self.segment_seconds = segment_seconds
        self.commit_callback = commit_callback or (lambda text: print(f"COMMIT: {text}"))
        self.status_callback = status_callback or (lambda text: print(f"STATUS: {text}"))
        self.audio_device = audio_device or "default"
        self.client_name = client_name
        self.frontend_name = frontend_name
        self.min_record_ms = min_record_ms  # minimum recording duration to avoid mis-triggers
        self.session_id: Optional[str] = None
        self.session_dir: Optional[Path] = None
        self.recorder: Optional[ArecordSegmentRecorder] = None
        self.segment_count = 0
        self.recording = False
        self.final_event = threading.Event()
        self.final_text = ""
        self._session_errored = False
        self._record_start_time: float = 0.0

    def connect(self) -> bool:
        """Connect to the backend and send hello.

        Returns True on success, False on failure (error is reported via status_callback).
        """
        try:
            self.client.connect()
            result = self.client.call("vibetype.hello", {"client": self.client_name, "protocol_version": 1}, timeout=5)
            if not result:
                self.status_callback("error: backend did not respond to hello")
                return False
            return True
        except (FileNotFoundError, ConnectionRefusedError):
            self.status_callback(
                f"error: cannot connect to backend at {self.client.socket_path}. "
                "Is vibetype-backend running?"
            )
            return False
        except (OSError, TimeoutError) as e:
            self.status_callback(f"error: connection failed: {e}")
            return False
        except RuntimeError as e:
            self.status_callback(f"error: hello handshake failed: {e}")
            return False

    def close(self) -> None:
        self.client.close()

    def model_status(self) -> Optional[dict]:
        return self._safe_call("vibetype.modelStatus", {})

    def ensure_model_ready(self) -> bool:
        status = self.model_status()
        if status is None:
            return False
        state = status.get("state")
        if state != "ready":
            self.status_callback(f"model {state}: {status.get('message', '')}")
            return False
        return True

    def begin_session(self, frontend: Optional[str] = None) -> bool:
        if not self.connect():
            return False
        if not self.ensure_model_ready():
            return False
        frontend = frontend or self.frontend_name
        self.session_id = str(uuid.uuid4())
        self.session_dir = ensure_runtime_session_dir(self.session_id)
        self.segment_count = 0
        self.final_text = ""
        self.final_event.clear()
        self._session_errored = False
        result = self._safe_call(
            "vibetype.startSession",
            {"session_id": self.session_id, "audio_format": AUDIO_FORMAT, "frontend": frontend},
        )
        if result is None:
            self.session_id = None
            self.session_dir = None
            return False
        return True

    def start_capture(self) -> bool:
        if not self.session_id or not self.session_dir:
            if not self.begin_session():
                return False
        if self.recording:
            return True
        assert self.session_dir is not None
        self._record_start_time = time.monotonic()
        self.recorder = ArecordSegmentRecorder(self.segment_seconds, self._submit_segment, self.audio_device)
        self.recorder.start(self.session_dir, self.segment_count)
        self.recording = True
        self.status_callback("recording")
        return True

    def stop_capture(self) -> None:
        if not self.recording:
            return
        assert self.recorder is not None
        self.status_callback("stopping")
        self.recorder.stop()
        self.recording = False
        self.status_callback("paused")

    def finish_session(self) -> bool:
        if not self.session_id:
            return False
        if self.recording:
            self.stop_capture()

        # Very short recordings (< min_record_ms) are likely mis-triggers.
        # Cancel the session instead of transcribing garbage.
        elapsed_ms = int((time.monotonic() - self._record_start_time) * 1000)
        if elapsed_ms < self.min_record_ms:
            self.status_callback(f"cancelled: recording too short ({elapsed_ms}ms)")
            self._safe_call("vibetype.cancelSession", {"session_id": self.session_id})
            self.final_event.set()
            return False

        self.status_callback("waiting final result")
        result = self._safe_call(
            "vibetype.finishSession",
            {"session_id": self.session_id, "segment_count": self.segment_count},
            timeout=5,
        )
        if result is None:
            self.status_callback("error: finishSession failed; session may be incomplete")
            self.final_event.set()  # Unblock waiters so they don't hang
            return False
        # If segments failed mid-session, the backend won't send finalResult.
        # Unblock waiters so they get an empty result instead of hanging.
        if self._session_errored:
            self.status_callback("error: session had errors, no final result will arrive")
            self.final_event.set()
        return True

    def start_recording(self) -> bool:
        return self.begin_session() and self.start_capture()

    def stop_recording(self) -> bool:
        return self.finish_session()

    def toggle_recording(self) -> None:
        if self.recording:
            self.stop_recording()
        else:
            self.start_recording()

    def submit_wav_for_test(self, wav_path: Path, wait_model: bool = True) -> str:
        if not self.connect():
            raise VibetypeError("cannot connect to backend")
        if wait_model:
            while True:
                status = self.model_status()
                if status is None:
                    raise VibetypeError("failed to query model status")
                state = status.get("state")
                self.status_callback(f"model {state}: {status.get('message', '')}")
                if state == "ready":
                    break
                if state == "error":
                    raise VibetypeError(f"model error: {status.get('message', '')}")
                time.sleep(1)
        self.session_id = str(uuid.uuid4())
        self.session_dir = ensure_runtime_session_dir(self.session_id)
        self.segment_count = 0
        self.final_event.clear()
        self._session_errored = False
        result = self._safe_call(
            "vibetype.startSession",
            {"session_id": self.session_id, "audio_format": AUDIO_FORMAT, "frontend": "fake-ibus"},
        )
        if result is None:
            raise VibetypeError("startSession failed")
        runtime_wav = copy_wav_to_runtime(wav_path, self.session_dir, 0)
        self._submit_segment(runtime_wav, 0, 0)
        self._safe_call("vibetype.finishSession", {"session_id": self.session_id, "segment_count": self.segment_count})
        if not self.final_event.wait(timeout=120):
            raise VibetypeError("timed out waiting for finalResult")
        return self.final_text

    def test_record_once(self, duration_seconds: int) -> str:
        self.session_id = str(uuid.uuid4())
        self.session_dir = ensure_runtime_session_dir(self.session_id)
        wav = self.session_dir / "record-test.wav"
        record_default_alsa_to_wav(wav, duration_seconds, self.audio_device)
        self.status_callback(f"recorded {wav}: {json.dumps(wav_stats(wav), ensure_ascii=False)}")
        return self.submit_wav_for_test(wav)

    def _safe_call(self, method: str, params: Optional[dict] = None, timeout: float = 30.0) -> Optional[dict]:
        """Call a JSON-RPC method, routing errors to status_callback.

        Returns the result dict on success, or None on error.
        Errors are reported via status_callback so both CLI and IBus display them.
        """
        try:
            return self.client.call(method, params, timeout)
        except TimeoutError:
            self.status_callback(f"error: {method} timed out after {timeout}s")
        except RuntimeError as e:
            self.status_callback(f"error: {method} failed: {e}")
        except (OSError, ConnectionError) as e:
            self.status_callback(f"error: connection to backend lost during {method}: {e}")
        return None

    def _submit_segment(self, path: Path, index: int, duration_ms: int) -> None:
        assert self.session_id is not None
        result = self._safe_call(
            "vibetype.transcribeSegment",
            {
                "session_id": self.session_id,
                "segment_index": index,
                "wav_path": str(path),
                "duration_ms": duration_ms,
            },
            timeout=120,
        )
        if result is not None:
            self.segment_count = max(self.segment_count, index + 1)
        else:
            self._session_errored = True

    def reload_config(self) -> Optional[dict]:
        """Call vibetype.reloadConfig on the backend. Returns result or None on error."""
        return self._safe_call("vibetype.reloadConfig", {}, timeout=10.0)

    def config_status(self) -> Optional[dict]:
        """Call vibetype.configStatus on the backend. Returns result or None on error."""
        return self._safe_call("vibetype.configStatus", {}, timeout=10.0)

    def polish_status(self) -> Optional[dict]:
        """Return optional Qwen polisher state."""
        return self._safe_call("vibetype.polishStatus", {}, timeout=10.0)

    def _on_notify(self, method: str, params: dict) -> None:
        if method == "vibetype.partialResult":
            self.status_callback(f"partial: {params.get('text', '')}")
        elif method == "vibetype.finalResult":
            self.final_text = params.get("text", "")
            self.status_callback("final result ready")
            self.commit_callback(self.final_text)
            self.final_event.set()
        elif method == "vibetype.error":
            self.status_callback(f"error: {params.get('message', params)}")
        elif method == "vibetype.modelStatusChanged":
            self.status_callback(f"model {params.get('state')}: {params.get('message', '')}")
        elif method == "vibetype.statusChanged":
            reason = params.get("reason")
            if reason == "config_reloaded":
                self.status_callback(f"config reloaded (revision {params.get('revision', '?')})")

