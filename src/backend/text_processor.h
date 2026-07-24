#pragma once

#include <map>
#include <string>

#include "config_manager.h"
#include "qwen_engine.h"

namespace vibetype {

// Text processing pipeline for ASR output.
//
// Final pipeline:   normalize → corrections → Qwen polish → corrections
// Partial pipeline: normalize → corrections  (no Qwen, fast path)
//
// Normalization (punctuation, spaces, CJK/ASCII) is performed by the caller
// (NormalizeTranscriptText in main.cc) before passing text to these methods.
class TextProcessor {
 public:
  TextProcessor() = default;

  // Process partial result (fast path, no Qwen).
  std::string ProcessPartial(const std::string& text,
                             const BackendConfig& cfg) const;

  // Process final result (may invoke Qwen if ready).
  // qwen may be nullptr (disabled or not started).
  std::string ProcessFinal(const std::string& text, const BackendConfig& cfg,
                           QwenEngine* qwen) const;

 private:
  // Apply correction map to text using UTF-8-aware word-boundary replacement.
  // CJK codepoint boundaries are treated as word boundaries so corrections
  // work inside continuous Chinese text without requiring spaces.
  static std::string ApplyCorrections(
      const std::string& text,
      const std::map<std::string, std::string>& corrections);
};

}  // namespace vibetype
