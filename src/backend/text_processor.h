#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vibetype {

// ─── Configuration ────────────────────────────────────────────────────────────

struct FillerConfig {
  bool enabled = true;
  std::vector<std::string> chinese_fillers = {"嗯", "呃"};
  std::vector<std::string> english_fillers = {"um", "uh"};
};

struct RepeatConfig {
  bool enabled = true;
  int min_phrase_chars = 2;  // minimum CJK chars for a phrase to be de-duped
  // Allowlist: only these CJK 2-char phrases are de-duplicated.
  // Populated by Defaults(); explicit empty array truly disables 2-char CJK de-dup.
  std::vector<std::string> cjk_allowlist;
  // Allowlist: only these English words are de-duplicated (function words).
  // Populated by Defaults(); explicit empty array truly disables English de-dup.
  std::vector<std::string> english_allowlist;
};

struct SelfRepairConfig {
  bool enabled = true;
  // Only high-confidence patterns: date, number, version
};

struct AliasEntry {
  std::string canonical;
  std::vector<std::string> aliases;
};

struct AliasConfig {
  bool enabled = true;
  std::vector<AliasEntry> entries;  // merged builtin + user entries
};

struct ProtectedSpanConfig {
  bool url = true;
  bool email = true;
  bool path = true;
  bool cli_option = true;
  bool version = true;
  bool backtick_code = true;
  bool tech_identifier = true;
};

struct TextProcessorConfig {
  FillerConfig filler;
  RepeatConfig repeat;
  SelfRepairConfig self_repair;
  AliasConfig alias;
  ProtectedSpanConfig protected_spans;

  // Load builtin defaults then merge user config on top.
  // On any parse failure, log an error and use defaults.
  // builtin_data_dir: directory containing data/text-processing.json and
  //                   data/computer_terms.json (normally the install prefix).
  // user_config_path: user override file (empty = use XDG default).
  static TextProcessorConfig Load(const std::string& builtin_data_dir = "",
                                  const std::string& user_config_path = "");

  // Return the default (built-in) configuration with no file I/O.
  static TextProcessorConfig Defaults();
};

// ─── Public interface ─────────────────────────────────────────────────────────

class TextProcessor {
 public:
  explicit TextProcessor(TextProcessorConfig cfg);

  // Access the current configuration.
  const TextProcessorConfig& Config() const { return cfg_; }

  // Apply the backend's existing punctuation/spacing normalization, while
  // shielding protected-span contents. Used for partial results.
  std::string ProcessPartial(const std::string& text) const;

  // Apply full processing pipeline. Protected spans are re-detected before
  // every mutating stage: normalization, filler/repeat/self-repair, aliases.
  // Used for final results.
  std::string ProcessFinal(const std::string& text) const;

 private:
  TextProcessorConfig cfg_;
};

}  // namespace vibetype
