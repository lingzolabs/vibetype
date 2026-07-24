#include "text_processor.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "xtils/logging/logger.h"

namespace vibetype {
namespace {

// Returns true if the byte at 'pos' in 'text' is a UTF-8 sequence boundary
// (i.e., not a continuation byte 10xxxxxx).
bool IsUtf8Boundary(const std::string& text, size_t pos) {
  if (pos == 0 || pos >= text.size()) return true;
  return (static_cast<unsigned char>(text[pos]) & 0xC0) != 0x80;
}

// Returns true if position 'pos' is a word-boundary for corrections.
// For ASCII characters: traditional word-boundary punctuation/space.
// For multi-byte (CJK etc.): every UTF-8 codepoint boundary is a word boundary,
// because Chinese text has no space-separated tokens.
bool IsWordBoundary(const std::string& text, size_t pos) {
  if (pos == 0 || pos >= text.size()) return true;
  const unsigned char c = static_cast<unsigned char>(text[pos]);
  // If the character is a UTF-8 continuation byte, this is mid-codepoint: not a boundary.
  if ((c & 0xC0) == 0x80) return false;
  // If the character is a multi-byte start (>= 0x80), it's a CJK/non-ASCII codepoint
  // boundary — treat it as a word boundary so corrections match inside Chinese sentences.
  if (c >= 0x80) return true;
  // ASCII: space, punctuation, or other delimiters.
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
         c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':' ||
         c == '(' || c == ')' || c == '[' || c == ']' || c == '"' ||
         c == '\'' || c == '/' || c == '-' || c == '_';
}

// Returns true if position just AFTER 'from' (i.e. pos + from.size()) is a word boundary.
bool IsRightBoundary(const std::string& text, size_t match_end) {
  return IsWordBoundary(text, match_end);
}

// Returns true if position 'pos' (start of match) follows a word boundary.
bool IsLeftBoundary(const std::string& text, size_t pos) {
  if (pos == 0) return true;
  // Look at the byte just before pos.
  // Walk back to the start of the preceding UTF-8 codepoint.
  size_t prev = pos - 1;
  while (prev > 0 && (static_cast<unsigned char>(text[prev]) & 0xC0) == 0x80) --prev;
  const unsigned char c = static_cast<unsigned char>(text[prev]);
  if (c >= 0x80) return true;  // non-ASCII preceding codepoint = boundary
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
         c == ',' || c == '.' || c == '!' || c == '?' || c == ';' || c == ':' ||
         c == '(' || c == ')' || c == '[' || c == ']' || c == '"' ||
         c == '\'' || c == '/' || c == '-' || c == '_';
}

}  // namespace

std::string TextProcessor::ApplyCorrections(
    const std::string& text,
    const std::map<std::string, std::string>& corrections) {
  if (corrections.empty() || text.empty()) return text;

  std::string result = text;

  // Sort corrections by key length descending so longer matches take priority.
  std::vector<std::pair<std::string, std::string>> sorted_corrections(
      corrections.begin(), corrections.end());
  std::sort(sorted_corrections.begin(), sorted_corrections.end(),
            [](const auto& a, const auto& b) {
              return a.first.size() > b.first.size();
            });

  for (const auto& [from, to] : sorted_corrections) {
    if (from.empty() || from == to) continue;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
      // Ensure we are at a valid UTF-8 boundary at the match start.
      if (!IsUtf8Boundary(result, pos)) { ++pos; continue; }
      const size_t match_end = pos + from.size();
      const bool left_ok  = IsLeftBoundary(result, pos);
      const bool right_ok = IsRightBoundary(result, match_end);
      if (left_ok && right_ok) {
        result.replace(pos, from.size(), to);
        pos += to.size();
      } else {
        pos += from.size();
      }
    }
  }
  return result;
}

std::string TextProcessor::ProcessPartial(const std::string& text,
                                          const BackendConfig& cfg) const {
  if (text.empty()) return text;
  // Partial: apply corrections only (normalization already applied upstream).
  return ApplyCorrections(text, cfg.custom_corrections);
}

std::string TextProcessor::ProcessFinal(const std::string& text,
                                        const BackendConfig& cfg,
                                        QwenEngine* qwen) const {
  if (text.empty()) return text;

  // Step 1: apply corrections to normalized text.
  std::string out = ApplyCorrections(text, cfg.custom_corrections);

  // Step 2: Qwen polish (if enabled and ready).
  const bool qwen_prompt_configured =
      !cfg.qwen_system_prompt.empty() || !cfg.qwen_prompt_template.empty();
  if (cfg.enable_qwen_polish && qwen_prompt_configured &&
      qwen != nullptr && qwen->Ready()) {
    const std::string polished = qwen->Polish(
        out, cfg.qwen_system_prompt, cfg.qwen_prompt_template,
        cfg.qwen_max_tokens, cfg.qwen_n_ctx, cfg.qwen_timeout_ms);
    // Reject obvious expansion/hallucination while allowing filler removal.
    const size_t max_safe_bytes = text.size() * 2 + 64;
    if (!polished.empty() && polished.size() <= max_safe_bytes) {
      out = polished;
      // Step 3: apply corrections again to fix any Qwen regressions.
      out = ApplyCorrections(out, cfg.custom_corrections);
    } else {
      LogW("text_processor: rejected empty, timed-out, or oversized Qwen output; keeping rule-based result");
    }
  }

  return out;
}

}  // namespace vibetype
