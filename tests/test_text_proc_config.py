#!/usr/bin/env python3
"""Standalone tests for vibetype_config.py (no external dependencies).

Run with:  python3 tests/test_text_proc_config.py
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

# Resolve common module location relative to this file.
_REPO_ROOT = Path(__file__).resolve().parents[1]
_COMMON_DIR = _REPO_ROOT / "frontend" / "common"
if str(_COMMON_DIR) not in sys.path:
    sys.path.insert(0, str(_COMMON_DIR))

from vibetype_config import (  # noqa: E402
    corrections_from_text,
    corrections_to_text,
    get_custom_corrections,
    get_enable_builtin_corrections,
    get_enable_qwen_polish,
    load_text_proc_config,
    normalize_corrections,
    patch_text_proc_config,
    save_text_proc_config,
    text_proc_config_path,
    validate_config,
)


class TestValidateConfig(unittest.TestCase):
    def test_empty_dict_is_valid(self):
        ok, err = validate_config({})
        self.assertTrue(ok, err)

    def test_all_schema_fields(self):
        ok, err = validate_config({
            "enable_builtin_corrections": True,
            "enable_qwen_polish": False,
            "custom_corrections": [{"from": "foo", "to": "bar"}],
        })
        self.assertTrue(ok, err)

    def test_bool_field_wrong_type(self):
        ok, err = validate_config({"enable_builtin_corrections": "yes"})
        self.assertFalse(ok)
        self.assertIn("enable_builtin_corrections", err)

    def test_corrections_object_form(self):
        ok, err = validate_config({"custom_corrections": {"abc": "ABC"}})
        self.assertTrue(ok, err)

    def test_corrections_array_form(self):
        ok, err = validate_config({"custom_corrections": [{"from": "abc", "to": "ABC"}]})
        self.assertTrue(ok, err)

    def test_corrections_wrong_type(self):
        ok, err = validate_config({"custom_corrections": "not-a-list"})
        self.assertFalse(ok)

    def test_corrections_missing_from(self):
        ok, err = validate_config({"custom_corrections": [{"to": "bar"}]})
        self.assertFalse(ok)
        self.assertIn("from", err)

    def test_corrections_too_long(self):
        ok, err = validate_config({
            "custom_corrections": [{"from": "a" * 257, "to": "b"}]
        })
        self.assertFalse(ok)

    def test_unknown_fields_preserved(self):
        ok, err = validate_config({"future_field": 42, "enable_builtin_corrections": True})
        self.assertTrue(ok, err)

    def test_not_a_dict(self):
        ok, err = validate_config([1, 2, 3])
        self.assertFalse(ok)


class TestNormalizeCorrections(unittest.TestCase):
    def test_array_form(self):
        result = normalize_corrections([{"from": "a", "to": "b"}])
        self.assertEqual(result, [{"from": "a", "to": "b"}])

    def test_object_form(self):
        result = normalize_corrections({"x": "y", "p": "q"})
        froms = {item["from"] for item in result}
        self.assertIn("x", froms)
        self.assertIn("p", froms)

    def test_none(self):
        self.assertEqual(normalize_corrections(None), [])

    def test_empty_list(self):
        self.assertEqual(normalize_corrections([]), [])

    def test_skip_invalid_items(self):
        result = normalize_corrections([{"from": "ok", "to": "yes"}, {"bad": "item"}])
        self.assertEqual(len(result), 1)


class TestCorrectionsTextCodec(unittest.TestCase):
    def test_roundtrip(self):
        # Simple case: no '=' in the 'from' part.
        original = [{"from": "foo", "to": "bar"}, {"from": "hello", "to": "world"}]
        encoded = corrections_to_text(original)
        decoded = corrections_from_text(encoded)
        self.assertEqual(decoded, original)

    def test_from_with_equals_not_roundtrippable(self):
        # If 'from' contains '=', the text codec cannot round-trip it
        # (first '=' is the separator). This is a known limitation.
        encoded = corrections_to_text([{"from": "a=b", "to": "c"}])
        decoded = corrections_from_text(encoded)
        # The from part is everything before the first '='
        self.assertEqual(decoded[0]["from"], "a")
        self.assertEqual(decoded[0]["to"], "b=c")

    def test_empty_lines_skipped(self):
        result = corrections_from_text("\n\nfoo=bar\n\n")
        self.assertEqual(result, [{"from": "foo", "to": "bar"}])

    def test_no_equals_skipped(self):
        result = corrections_from_text("no-equals\nfoo=bar")
        self.assertEqual(result, [{"from": "foo", "to": "bar"}])

    def test_empty_from_skipped(self):
        result = corrections_from_text("=bar\nfoo=baz")
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["from"], "foo")

    def test_value_with_equals(self):
        result = corrections_from_text("key=val=ue")
        self.assertEqual(result, [{"from": "key", "to": "val=ue"}])

    def test_whitespace_stripped(self):
        result = corrections_from_text("  foo  =  bar  ")
        self.assertEqual(result, [{"from": "foo", "to": "bar"}])

    def test_to_text_empty(self):
        self.assertEqual(corrections_to_text([]), "")


class TestLoadSave(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.cfg_path = Path(self.tmpdir.name) / "text-processing.json"

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_load_missing_file_returns_empty(self):
        data = load_text_proc_config(self.cfg_path)
        self.assertEqual(data, {})

    def test_save_and_load_roundtrip(self):
        original = {
            "enable_builtin_corrections": False,
            "enable_qwen_polish": True,
            "custom_corrections": [{"from": "aaa", "to": "bbb"}],
            "unknown_future_field": "preserved",
        }
        save_text_proc_config(original, self.cfg_path)
        loaded = load_text_proc_config(self.cfg_path)
        self.assertEqual(loaded["enable_builtin_corrections"], False)
        self.assertEqual(loaded["enable_qwen_polish"], True)
        self.assertEqual(loaded["custom_corrections"], [{"from": "aaa", "to": "bbb"}])
        # Unknown fields must be preserved
        self.assertEqual(loaded["unknown_future_field"], "preserved")

    def test_save_creates_parent_dirs(self):
        deep_path = Path(self.tmpdir.name) / "a" / "b" / "c" / "text-processing.json"
        save_text_proc_config({}, deep_path)
        self.assertTrue(deep_path.exists())

    def test_save_invalid_raises(self):
        with self.assertRaises(ValueError):
            save_text_proc_config({"enable_builtin_corrections": "not-bool"}, self.cfg_path)

    def test_load_invalid_json_raises(self):
        self.cfg_path.write_text("not json", encoding="utf-8")
        with self.assertRaises(ValueError):
            load_text_proc_config(self.cfg_path)

    def test_load_invalid_schema_raises(self):
        self.cfg_path.write_text('{"enable_builtin_corrections": "bad"}', encoding="utf-8")
        with self.assertRaises(ValueError):
            load_text_proc_config(self.cfg_path)

    def test_atomic_write_uses_temp_then_rename(self):
        # After save, no .tmp file should remain
        save_text_proc_config({"enable_qwen_polish": True}, self.cfg_path)
        tmp = self.cfg_path.parent / (self.cfg_path.name + ".tmp")
        # The tmp file should be gone after successful write
        remaining = list(self.cfg_path.parent.glob("*.tmp"))
        self.assertEqual(remaining, [])

    def test_file_is_valid_utf8_json(self):
        save_text_proc_config({"enable_qwen_polish": False}, self.cfg_path)
        text = self.cfg_path.read_text(encoding="utf-8")
        data = json.loads(text)
        self.assertIsInstance(data, dict)


class TestPatchConfig(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.cfg_path = Path(self.tmpdir.name) / "text-processing.json"

    def tearDown(self):
        self.tmpdir.cleanup()

    def test_patch_creates_file(self):
        result = patch_text_proc_config(
            {"enable_builtin_corrections": False},
            self.cfg_path,
        )
        self.assertFalse(result["enable_builtin_corrections"])
        self.assertTrue(self.cfg_path.exists())

    def test_patch_preserves_unknown_fields(self):
        # Pre-populate with an unknown field.
        self.cfg_path.write_text(
            json.dumps({"unknown": "keep_me", "enable_qwen_polish": False}),
            encoding="utf-8",
        )
        result = patch_text_proc_config(
            {"enable_builtin_corrections": True},
            self.cfg_path,
        )
        self.assertEqual(result["unknown"], "keep_me")
        self.assertEqual(result["enable_builtin_corrections"], True)
        # Verify file also has it
        loaded = load_text_proc_config(self.cfg_path)
        self.assertEqual(loaded["unknown"], "keep_me")

    def test_patch_corrections_normalizes_object_form(self):
        # Existing file has object form corrections; patch should write array form.
        self.cfg_path.write_text(
            json.dumps({"custom_corrections": {"old_wrong": "old_correct"}}),
            encoding="utf-8",
        )
        result = patch_text_proc_config(
            {"custom_corrections": [{"from": "new", "to": "ok"}]},
            self.cfg_path,
        )
        self.assertIsInstance(result["custom_corrections"], list)
        self.assertEqual(result["custom_corrections"][0]["from"], "new")

    def test_patch_idempotent(self):
        patch_text_proc_config({"enable_qwen_polish": True}, self.cfg_path)
        patch_text_proc_config({"enable_qwen_polish": True}, self.cfg_path)
        loaded = load_text_proc_config(self.cfg_path)
        self.assertTrue(loaded["enable_qwen_polish"])


class TestConvenienceAccessors(unittest.TestCase):
    def test_defaults(self):
        self.assertTrue(get_enable_builtin_corrections({}))
        self.assertFalse(get_enable_qwen_polish({}))
        self.assertEqual(get_custom_corrections({}), [])

    def test_with_values(self):
        data = {
            "enable_builtin_corrections": False,
            "enable_qwen_polish": True,
            "custom_corrections": [{"from": "a", "to": "b"}],
        }
        self.assertFalse(get_enable_builtin_corrections(data))
        self.assertTrue(get_enable_qwen_polish(data))
        self.assertEqual(get_custom_corrections(data), [{"from": "a", "to": "b"}])

    def test_corrections_object_form_via_accessor(self):
        data = {"custom_corrections": {"x": "y"}}
        result = get_custom_corrections(data)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["from"], "x")
        self.assertEqual(result[0]["to"], "y")


if __name__ == "__main__":
    unittest.main(verbosity=2)
