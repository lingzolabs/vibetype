"""Vibetype shared text-processing configuration helpers.

Config file: ${XDG_CONFIG_HOME:-~/.config}/vibetype/text-processing.json

Schema (only these fields are validated; all unknown fields are preserved):
  enable_builtin_corrections  bool   – enable built-in computer-term corrections
  enable_qwen_polish          bool   – enable Qwen LLM polish pass
  custom_corrections          list   – [{from: str, to: str}, ...]
                                       (object form {from: to, ...} is also
                                        accepted for reading, but we always
                                        write the array form)
"""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any


# ── Path helpers ──────────────────────────────────────────────────────────────

def config_dir() -> Path:
    """Return the vibetype config directory (~/.config/vibetype or XDG)."""
    base = Path(os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
    return base / "vibetype"


def text_proc_config_path() -> Path:
    """Return the canonical path for text-processing.json."""
    return config_dir() / "text-processing.json"


# ── Schema constants ──────────────────────────────────────────────────────────

_KNOWN_BOOL_FIELDS = frozenset(["enable_builtin_corrections", "enable_qwen_polish"])
_MAX_CORRECTIONS = 500
_MAX_CORRECTION_LEN = 256


# ── Validation ────────────────────────────────────────────────────────────────

def validate_config(data: Any) -> tuple[bool, str]:
    """Validate config dict. Returns (ok, error_message).

    Only the three schema fields are validated; unknown fields are allowed.
    """
    if not isinstance(data, dict):
        return False, "config must be a JSON object"

    for field in _KNOWN_BOOL_FIELDS:
        if field in data and not isinstance(data[field], bool):
            return False, f"{field!r} must be a boolean"

    if "custom_corrections" in data:
        cc = data["custom_corrections"]
        if isinstance(cc, dict):
            if len(cc) > _MAX_CORRECTIONS:
                return False, f"custom_corrections must have at most {_MAX_CORRECTIONS} entries"
            for k, v in cc.items():
                if not isinstance(k, str) or not isinstance(v, str):
                    return False, "custom_corrections object keys and values must be strings"
                if not k:
                    return False, "custom_corrections keys must not be empty"
                if len(k) > _MAX_CORRECTION_LEN or len(v) > _MAX_CORRECTION_LEN:
                    return False, f"custom_corrections keys and values must be at most {_MAX_CORRECTION_LEN} characters"
        elif isinstance(cc, list):
            if len(cc) > _MAX_CORRECTIONS:
                return False, f"custom_corrections must have at most {_MAX_CORRECTIONS} entries"
            for i, item in enumerate(cc):
                if not isinstance(item, dict):
                    return False, f"custom_corrections[{i}] must be an object"
                if not isinstance(item.get("from"), str):
                    return False, f"custom_corrections[{i}].from must be a string"
                if not isinstance(item.get("to"), str):
                    return False, f"custom_corrections[{i}].to must be a string"
                if not item["from"]:
                    return False, f"custom_corrections[{i}].from must not be empty"
                if len(item["from"]) > _MAX_CORRECTION_LEN:
                    return False, f"custom_corrections[{i}].from too long (max {_MAX_CORRECTION_LEN})"
                if len(item["to"]) > _MAX_CORRECTION_LEN:
                    return False, f"custom_corrections[{i}].to too long (max {_MAX_CORRECTION_LEN})"
        else:
            return False, "custom_corrections must be an array or object"

    return True, ""


# ── Normalisation ─────────────────────────────────────────────────────────────

def normalize_corrections(cc: Any) -> list[dict[str, str]]:
    """Normalise custom_corrections to canonical array form [{from, to}, ...]."""
    if cc is None:
        return []
    if isinstance(cc, dict):
        return [{"from": k, "to": v} for k, v in cc.items()
                if isinstance(k, str) and k and isinstance(v, str)]
    if isinstance(cc, list):
        result = []
        for item in cc:
            if (isinstance(item, dict) and isinstance(item.get("from"), str)
                    and item["from"] and isinstance(item.get("to"), str)):
                result.append({"from": item["from"], "to": item["to"]})
        return result
    return []


# ── Read ──────────────────────────────────────────────────────────────────────

def load_text_proc_config(path: Path | None = None) -> dict:
    """Load text-processing.json. Returns empty dict if file does not exist.

    Raises ValueError on JSON/schema parse error.
    Does NOT apply defaults – returns exactly what is stored (plus validation).
    Unknown fields are preserved as-is.
    """
    if path is None:
        path = text_proc_config_path()
    if not path.exists():
        return {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"cannot read {path}: {exc}") from exc
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"JSON parse error in {path}: {exc}") from exc
    ok, err = validate_config(data)
    if not ok:
        raise ValueError(f"invalid config in {path}: {err}")
    return data


# ── Atomic save ───────────────────────────────────────────────────────────────

def save_text_proc_config(data: dict, path: Path | None = None) -> None:
    """Atomically save config to path using temp file + fsync + rename.

    Unknown fields already present in *data* are preserved (caller is
    responsible for merging before calling this function).
    Raises ValueError on validation error, OSError on I/O error.
    """
    if path is None:
        path = text_proc_config_path()

    ok, err = validate_config(data)
    if not ok:
        raise ValueError(f"invalid config: {err}")

    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    encoded = text.encode("utf-8")

    # Write to a temp file in the same directory so rename is atomic.
    fd, tmp_path_str = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
    tmp_path = Path(tmp_path_str)
    try:
        with os.fdopen(fd, "wb") as fh:
            fh.write(encoded)
            fh.flush()
            os.fsync(fh.fileno())
        tmp_path.rename(path)
    except Exception:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise


# ── Merge helpers ─────────────────────────────────────────────────────────────

def patch_text_proc_config(
    patches: dict,
    path: Path | None = None,
) -> dict:
    """Load existing config, apply patches, validate, and atomically save.

    Returns the merged dict after saving.
    Unknown fields in the existing file are preserved.
    patches keys: enable_builtin_corrections, enable_qwen_polish,
                  custom_corrections (as list).
    """
    existing = load_text_proc_config(path)
    merged = dict(existing)

    if "enable_builtin_corrections" in patches:
        merged["enable_builtin_corrections"] = bool(patches["enable_builtin_corrections"])
    if "enable_qwen_polish" in patches:
        merged["enable_qwen_polish"] = bool(patches["enable_qwen_polish"])
    if "custom_corrections" in patches:
        merged["custom_corrections"] = normalize_corrections(patches["custom_corrections"])

    save_text_proc_config(merged, path)
    return merged


# ── Convenience accessors (with defaults) ────────────────────────────────────

def get_enable_builtin_corrections(data: dict) -> bool:
    """Return enable_builtin_corrections with default True."""
    return bool(data.get("enable_builtin_corrections", True))


def get_enable_qwen_polish(data: dict) -> bool:
    """Return enable_qwen_polish with default False."""
    return bool(data.get("enable_qwen_polish", False))


def get_custom_corrections(data: dict) -> list[dict[str, str]]:
    """Return custom_corrections as canonical array form."""
    return normalize_corrections(data.get("custom_corrections"))


# ── Custom-corrections text codec (for UI multi-line editor) ─────────────────

def corrections_to_text(corrections: list[dict[str, str]]) -> str:
    """Encode corrections list to multi-line text: each line is 'from=to'."""
    lines = []
    for item in corrections:
        f = item.get("from", "")
        t = item.get("to", "")
        # Escape literal '=' in the "from" part by encoding as first '=' separator
        lines.append(f"{f}={t}")
    return "\n".join(lines)


def corrections_from_text(text: str) -> list[dict[str, str]]:
    """Parse multi-line 'from=to' text back to corrections list.

    Lines that do not contain '=' are silently skipped.
    Empty lines are skipped.
    Leading/trailing whitespace on each line is stripped.
    The split is on the first '=' only so that values may contain '='.
    """
    result = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if "=" not in line:
            continue
        from_part, to_part = line.split("=", 1)
        from_part = from_part.strip()
        to_part = to_part.strip()
        if not from_part:
            continue
        result.append({"from": from_part, "to": to_part})
    return result
