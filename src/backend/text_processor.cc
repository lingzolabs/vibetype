// src/backend/text_processor.cc
// Deterministic text processing for Vibetype backend.
// No LLM dependencies; pure rule-based normalization and cleanup.

#include "text_processor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "xtils/logging/logger.h"
#include "xtils/utils/json.h"

#define TP_LOGE(...) LogE(__VA_ARGS__)
#define TP_LOGW(...) LogW(__VA_ARGS__)
#define TP_LOGI(...) LogI(__VA_ARGS__)

namespace vibetype {

// ─── Internal: protected span ────────────────────────────────────────────────

struct ProtectedSpan {
  size_t begin = 0;  // byte offset, inclusive
  size_t end = 0;    // byte offset, exclusive
};

// ─── UTF-8 utilities ──────────────────────────────────────────────────────────

struct Utf8Char {
  char bytes[5] = {};  // null-terminated UTF-8 sequence
  uint32_t codepoint = 0;
  size_t byte_len = 0;
};

// Validate that bytes p[1..n-1] are all continuation bytes (0x80..0xBF).
static bool ValidContinuations(const char* p, size_t n) {
  for (size_t i = 1; i < n; ++i) {
    const unsigned char b = static_cast<unsigned char>(p[i]);
    if ((b & 0xC0) != 0x80) return false;
  }
  return true;
}

static Utf8Char DecodeUtf8(const char* p, size_t remaining) {
  Utf8Char out;
  if (remaining == 0) return out;
  const unsigned char c = static_cast<unsigned char>(p[0]);
  if ((c & 0x80) == 0) {
    // Single-byte ASCII
    out.byte_len = 1;
    out.codepoint = c;
  } else if ((c & 0xE0) == 0xC0 && remaining >= 2 && ValidContinuations(p, 2)) {
    out.byte_len = 2;
    out.codepoint = ((c & 0x1F) << 6) |
                    (static_cast<unsigned char>(p[1]) & 0x3F);
  } else if ((c & 0xF0) == 0xE0 && remaining >= 3 && ValidContinuations(p, 3)) {
    out.byte_len = 3;
    out.codepoint = ((c & 0x0F) << 12) |
                    ((static_cast<unsigned char>(p[1]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(p[2]) & 0x3F);
  } else if ((c & 0xF8) == 0xF0 && remaining >= 4 && ValidContinuations(p, 4)) {
    out.byte_len = 4;
    out.codepoint = ((c & 0x07) << 18) |
                    ((static_cast<unsigned char>(p[1]) & 0x3F) << 12) |
                    ((static_cast<unsigned char>(p[2]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(p[3]) & 0x3F);
  } else {
    // Invalid or truncated byte sequence; consume exactly one byte as replacement
    // so downstream ASCII bytes are never swallowed.
    out.byte_len = 1;
    out.codepoint = 0xFFFD;
  }
  std::memcpy(out.bytes, p, out.byte_len);
  out.bytes[out.byte_len] = '\0';
  return out;
}

static bool IsCjkIdeograph(uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x2A6DF) ||
         (cp >= 0x2A700 && cp <= 0x2B73F);
}

static bool IsKana(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x30FF);  // Hiragana + Katakana
}

static bool IsHangul(uint32_t cp) {
  return (cp >= 0xAC00 && cp <= 0xD7AF) ||
         (cp >= 0x1100 && cp <= 0x11FF);
}

static bool IsCjkAny(uint32_t cp) {
  return IsCjkIdeograph(cp) || IsKana(cp) || IsHangul(cp);
}

// Only CJK ideographs and kana use full-width punctuation (Korean uses half-width)
static bool NeedsFullWidthPunct(uint32_t cp) {
  return IsCjkIdeograph(cp) || IsKana(cp);
}

static bool IsAsciiLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

static bool IsAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool IsWordBoundary(char c) {
  // Treat as word boundary: space, punctuation, or start/end of string
  if (IsAsciiSpace(c)) return true;
  if (IsAsciiLetter(c) || IsAsciiDigit(c) || c == '_') return false;
  return true;  // punctuation, CJK byte lead, etc.
}

static std::string ToLower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// ─── Protected span detection ────────────────────────────────────────────────
//
// Strategy: scan left to right; when a span is entered, skip to its end.
// Order of precedence: backtick > URL > email > path > CLI option > version > tech-id

struct SpanList {
  std::vector<ProtectedSpan> spans;

  void Add(size_t begin, size_t end) {
    if (end <= begin) return;
    // Merge or append (spans come in sorted order from a left-to-right scan)
    if (!spans.empty() && spans.back().end >= begin) {
      spans.back().end = std::max(spans.back().end, end);
    } else {
      spans.push_back({begin, end});
    }
  }

  bool Covers(size_t pos) const {
    for (const auto& s : spans) {
      if (pos >= s.begin && pos < s.end) return true;
    }
    return false;
  }
};

// Returns true if pos is inside any existing span
static bool InsideSpan(const SpanList& sl, size_t pos) {
  return sl.Covers(pos);
}

// Try to match a URL starting at pos in text. Returns end offset or 0.
// Supports ASCII URLs and Unicode IRI (IDN host, Unicode path segments).
static size_t TryMatchUrl(const std::string& text, size_t pos) {
  static const char* prefixes[] = {"https://", "http://", "ftp://", nullptr};
  for (int i = 0; prefixes[i]; ++i) {
    const size_t plen = std::strlen(prefixes[i]);
    if (text.size() - pos < plen) continue;
    if (text.compare(pos, plen, prefixes[i]) == 0) {
      // Consume URL / IRI chars.
      // Allowed: any non-space byte EXCEPT bare ASCII sentence-end punctuation
      // that is clearly NOT part of the URL.
      // We allow all UTF-8 multi-byte sequences (>= 0x80) so Unicode IRI
      // domains and paths (e.g. https://例子.测试/路径) are fully protected.
      size_t j = pos + plen;
      while (j < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[j]);
        // ASCII space terminates URL
        if (IsAsciiSpace(text[j])) break;
        // Multi-byte UTF-8: only part of URL if no unstripped ASCII sentence
        // punctuation immediately precedes (i.e. we are still in the URL body).
        if (c >= 0x80) { ++j; continue; }
        ++j;
      }
      // Strip trailing ASCII sentence punctuation that is unlikely part of URL.
      // Also strip trailing ASCII sentence-punctuation followed by multi-byte
      // (CJK) text: the CJK is definitely not part of the URL.
      // Step 1: back up past any trailing multi-byte (CJK) suffix that follows
      // an ASCII comma/period (the CJK text is the surrounding sentence).
      // We look for pattern: [word chars] [ASCII punct] [multi-byte...] at end.
      {
        // Find rightmost ASCII punctuation that has only multi-byte after it
        size_t scan = j;
        while (scan > pos + plen) {
          // Walk backwards: if all suffix after some position is multi-byte,
          // and the char at that position is ASCII sentence punct, cut there.
          const unsigned char b = static_cast<unsigned char>(text[scan - 1]);
          if (b >= 0x80) { --scan; continue; }  // still in multi-byte suffix
          // ASCII char at scan-1: is it sentence punctuation?
          const char lc = static_cast<char>(b);
          if (lc == ',' || lc == '.' || lc == '!' || lc == '?' ||
              lc == ';' || lc == ':' || lc == ')' || lc == ']') {
            // Everything from scan-1 onward: punct + CJK. Cut before the punct.
            j = scan - 1;
          }
          break;  // stop at first ASCII char from right
        }
      }
      // Step 2: strip remaining trailing ASCII sentence punctuation
      while (j > pos + plen) {
        const unsigned char last = static_cast<unsigned char>(text[j - 1]);
        if (last >= 0x80) break;
        const char lc = static_cast<char>(last);
        if (lc == '.' || lc == ',' || lc == '!' || lc == '?' ||
            lc == ';' || lc == ':' || lc == ')' || lc == ']') {
          --j;
        } else {
          break;
        }
      }
      if (j > pos + plen) return j;
      return 0;
    }
  }
  return 0;
}

// Try to match an email address starting at pos. Returns end offset or 0.
static size_t TryMatchEmail(const std::string& text, size_t pos) {
  // Simple heuristic: word chars @ word chars . word chars
  // Require previous char to be word boundary
  if (pos > 0 && (IsAsciiLetter(text[pos - 1]) || IsAsciiDigit(text[pos - 1]) ||
                  text[pos - 1] == '_')) {
    return 0;  // continuation of word
  }
  // Scan local part: [a-zA-Z0-9._+\-]
  size_t j = pos;
  while (j < text.size() && (IsAsciiLetter(text[j]) || IsAsciiDigit(text[j]) ||
                              text[j] == '.' || text[j] == '_' || text[j] == '+' ||
                              text[j] == '-')) {
    ++j;
  }
  if (j == pos || j >= text.size() || text[j] != '@') return 0;
  ++j;  // skip @
  if (j >= text.size() || !IsAsciiLetter(text[j])) return 0;
  // Scan domain: [a-zA-Z0-9.\-]
  while (j < text.size() && (IsAsciiLetter(text[j]) || IsAsciiDigit(text[j]) ||
                              text[j] == '.' || text[j] == '-')) {
    ++j;
  }
  // Must contain a dot in domain and TLD must be letters
  // Simple check: ensure there's a dot somewhere after the @
  const size_t at_pos = text.find('@', pos);
  if (at_pos == std::string::npos) return 0;
  const size_t dot_pos = text.find('.', at_pos + 1);
  if (dot_pos == std::string::npos || dot_pos >= j) return 0;
  return j;
}

// Try to match a file path starting at pos. Returns end offset or 0.
// Matches: /absolute/path, ~/path, src/backend/file.h (relative with slash+dot)
static size_t TryMatchPath(const std::string& text, size_t pos) {
  if (pos > 0) {
    const char prev = text[pos - 1];
    if (IsAsciiLetter(prev) || IsAsciiDigit(prev) || prev == '_') return 0;
  }
  const size_t len = text.size();

  auto IsPathChar = [](char c) {
    return IsAsciiLetter(c) || IsAsciiDigit(c) ||
           c == '/' || c == '.' || c == '_' || c == '-' || c == '+' || c == ':';
  };

  // Absolute path: starts with /
  if (text[pos] == '/' && pos + 1 < len && IsPathChar(text[pos + 1])) {
    size_t j = pos + 1;
    while (j < len && IsPathChar(text[j])) ++j;
    if (j > pos + 1) return j;
    return 0;
  }

  // Home path: ~/
  if (pos + 1 < len && text[pos] == '~' && text[pos + 1] == '/') {
    size_t j = pos + 2;
    while (j < len && IsPathChar(text[j])) ++j;
    return j > pos + 2 ? j : 0;
  }

  // Relative path with slash: word/word[.ext] – require at least one slash and a dot or multiple components
  if (IsAsciiLetter(text[pos]) || IsAsciiDigit(text[pos]) || text[pos] == '_') {
    size_t j = pos;
    bool has_slash = false;
    bool has_dot = false;
    while (j < len && IsPathChar(text[j])) {
      if (text[j] == '/') has_slash = true;
      if (text[j] == '.') has_dot = true;
      ++j;
    }
    // Require: has slash AND (has dot OR longer than 3 components)
    // Also require it starts with a letter/digit (no pure number like 1.2.3)
    if (has_slash && (has_dot || j - pos > 6)) {
      // Exclude pure version numbers like "1.2/3"
      bool pure_version = true;
      for (size_t k = pos; k < j; ++k) {
        if (!IsAsciiDigit(text[k]) && text[k] != '.' && text[k] != '/') {
          pure_version = false;
          break;
        }
      }
      if (!pure_version) return j;
    }
    return 0;
  }

  return 0;
}

// Try to match a CLI option: --flag or -f (short) at pos.
static size_t TryMatchCliOption(const std::string& text, size_t pos) {
  if (pos > 0) {
    const unsigned char prev = static_cast<unsigned char>(text[pos - 1]);
    if (IsAsciiLetter(static_cast<char>(prev)) ||
        IsAsciiDigit(static_cast<char>(prev)) || prev == '_' || prev == '-') {
      return 0;  // reject embedded forms such as self--contained
    }
  }
  const size_t len = text.size();
  if (pos >= len || text[pos] != '-') return 0;

  // Long option: --word[-word]*
  if (pos + 1 < len && text[pos + 1] == '-') {
    size_t j = pos + 2;
    while (j < len && (IsAsciiLetter(text[j]) || IsAsciiDigit(text[j]) ||
                       text[j] == '-' || text[j] == '_')) {
      ++j;
    }
    if (j > pos + 2) return j;
    return 0;
  }

  // Short option: -X (single letter)
  if (pos + 1 < len && IsAsciiLetter(text[pos + 1])) {
    return pos + 2;
  }

  return 0;
}

// Try to match a version number at pos: [v]N.N[.N]* not preceded by a letter
static size_t TryMatchVersion(const std::string& text, size_t pos) {
  if (pos > 0) {
    const char prev = text[pos - 1];
    if (IsAsciiLetter(prev) || IsAsciiDigit(prev) || prev == '_' || prev == '.') return 0;
  }
  const size_t len = text.size();
  size_t j = pos;

  // Optional leading 'v' or 'V'
  if (j < len && (text[j] == 'v' || text[j] == 'V')) {
    // next char must be digit
    if (j + 1 < len && IsAsciiDigit(text[j + 1])) {
      ++j;
    } else {
      return 0;
    }
  }

  if (j >= len || !IsAsciiDigit(text[j])) return 0;

  // Consume N.N[.N]*  or  N.N.N.N (IP)
  size_t start_j = j;
  int dot_count = 0;
  while (j < len) {
    if (IsAsciiDigit(text[j])) {
      ++j;
    } else if (text[j] == '.' && j + 1 < len && IsAsciiDigit(text[j + 1])) {
      ++dot_count;
      ++j;
    } else if (text[j] == ':' && dot_count == 3 && j + 1 < len && IsAsciiDigit(text[j + 1])) {
      // IP:port
      ++j;
      while (j < len && IsAsciiDigit(text[j])) ++j;
      break;
    } else {
      break;
    }
  }

  // Require at least one dot to be a version/IP
  if (dot_count == 0) return 0;

  // Must not be followed by a letter (e.g. "1.2abc" is not a version)
  if (j < len && IsAsciiLetter(text[j])) return 0;

  return j;
}

// Try to match a backtick code span: `...`
static size_t TryMatchBacktick(const std::string& text, size_t pos) {
  if (text[pos] != '`') return 0;
  const size_t end = text.find('`', pos + 1);
  if (end == std::string::npos) return 0;
  return end + 1;
}

// Try to match a tech identifier with dots: word.word[.word]*
// e.g. vibetype.finalResult, com.example.package
// Must start at a word boundary, contain at least one dot, all parts are word chars.
static size_t TryMatchTechIdentifier(const std::string& text, size_t pos) {
  if (pos > 0) {
    const char prev = text[pos - 1];
    if (IsAsciiLetter(prev) || IsAsciiDigit(prev) || prev == '_') return 0;
  }
  if (!IsAsciiLetter(text[pos]) && text[pos] != '_') return 0;

  const size_t len = text.size();
  size_t j = pos;
  int dot_count = 0;

  while (j < len) {
    // Consume word chars
    if (IsAsciiLetter(text[j]) || IsAsciiDigit(text[j]) || text[j] == '_') {
      ++j;
    } else if (text[j] == '.' && j + 1 < len &&
               (IsAsciiLetter(text[j + 1]) || IsAsciiDigit(text[j + 1]) || text[j + 1] == '_')) {
      ++dot_count;
      ++j;
    } else {
      break;
    }
  }

  if (dot_count == 0) return 0;

  // Must not match a plain domain like "github.com" that could also be a URL domain –
  // but we still want to protect it. The key rule: at least one dot, all alphanumeric+underscore.
  // We require at least 2 chars before first dot to avoid matching "v.1" etc.
  const size_t first_dot = text.find('.', pos);
  if (first_dot == std::string::npos || first_dot - pos < 2) return 0;

  return j;
}

// Build the full protected span list for a text.
static std::vector<ProtectedSpan> FindProtectedSpansImpl(
    const ProtectedSpanConfig& cfg, const std::string& text) {
  SpanList sl;
  const size_t len = text.size();

  for (size_t i = 0; i < len; ) {
    if (InsideSpan(sl, i)) {
      ++i;
      continue;
    }

    size_t end = 0;

    // 1. Backtick code (highest priority)
    if (cfg.backtick_code && text[i] == '`') {
      end = TryMatchBacktick(text, i);
      if (end) { sl.Add(i, end); i = end; continue; }
    }

    // 2. URL
    if (cfg.url && i + 4 < len &&
        (text[i] == 'h' || text[i] == 'f')) {
      end = TryMatchUrl(text, i);
      if (end) { sl.Add(i, end); i = end; continue; }
    }

    // 3. Email (must check before path, since user@host looks like a path component)
    if (cfg.email &&
        (IsAsciiLetter(text[i]) || IsAsciiDigit(text[i]))) {
      end = TryMatchEmail(text, i);
      if (end) { sl.Add(i, end); i = end; continue; }
    }

    // 4. Path
    if (cfg.path &&
        (text[i] == '/' || text[i] == '~' ||
         IsAsciiLetter(text[i]) || IsAsciiDigit(text[i]))) {
      end = TryMatchPath(text, i);
      if (end) { sl.Add(i, end); i = end; continue; }
    }

    // 5. CLI option
    if (cfg.cli_option && text[i] == '-') {
      end = TryMatchCliOption(text, i);
      if (end) { sl.Add(i, end); i = end; continue; }
    }

    // 6. Version / IP
    if (cfg.version &&
        (IsAsciiDigit(text[i]) || text[i] == 'v' || text[i] == 'V')) {
      end = TryMatchVersion(text, i);
      if (end) { sl.Add(i, end); i = end; continue; }
    }

    // 7. Tech identifier with dots (e.g. vibetype.finalResult)
    if (cfg.tech_identifier && IsAsciiLetter(text[i])) {
      end = TryMatchTechIdentifier(text, i);
      if (end) { sl.Add(i, end); i = end; continue; }
    }

    ++i;
  }

  return sl.spans;
}

// ─── Punctuation normalization (from main.cc, extended) ──────────────────────

static bool IsHalfPunct(const std::string& s) {
  return s == "." || s == "," || s == "!" || s == "?" || s == ";" || s == ":";
}

static bool IsFullPunct(const std::string& s) {
  return s == "。" || s == "，" || s == "！" || s == "？" || s == "；" ||
         s == "：" || s == "、";
}

static bool IsAnyPunct(const std::string& s) {
  return IsHalfPunct(s) || IsFullPunct(s);
}

// Build token list: each token is a UTF-8 character
struct Token {
  std::string text;
  uint32_t cp = 0;
  size_t byte_pos = 0;  // byte offset in original string
};

static std::vector<Token> Tokenize(const std::string& text) {
  std::vector<Token> tokens;
  for (size_t i = 0; i < text.size(); ) {
    Utf8Char c = DecodeUtf8(text.data() + i, text.size() - i);
    tokens.push_back({std::string(c.bytes, c.byte_len), c.codepoint, i});
    i += c.byte_len;
  }
  return tokens;
}

static bool ContainsFullWidthPunctChar(const std::vector<Token>& tokens) {
  for (const auto& t : tokens) {
    if (NeedsFullWidthPunct(t.cp)) return true;
  }
  return false;
}

static std::string NormPunct(const std::string& tok, bool cjk_text) {
  if (cjk_text) {
    if (tok == ".")  return "。";
    if (tok == ",")  return "，";
    if (tok == "!")  return "！";
    if (tok == "?")  return "？";
    if (tok == ";")  return "；";
    if (tok == ":")  return "：";
    return tok;
  } else {
    if (tok == "。") return ".";
    if (tok == "，") return ",";
    if (tok == "！") return "!";
    if (tok == "？") return "?";
    if (tok == "；") return ";";
    if (tok == "：") return ":";
    if (tok == "、") return ",";
    return tok;
  }
}

static bool IsDigitStr(const std::string& s) {
  return s.size() == 1 && IsAsciiDigit(s[0]);
}

// Trim trailing ASCII spaces
static std::string TrimRight(std::string s) {
  while (!s.empty() && IsAsciiSpace(s.back())) s.pop_back();
  return s;
}

// ─── Core normalization ───────────────────────────────────────────────────────
//
// LegacyCoreNormalize is a verbatim port of master's NormalizeTranscriptText.
// It applies punctuation normalization and spacing with the original algorithm.
// cjk_text must be determined by the caller (from the original input text so
// that CJK chars in protected spans are counted correctly).
//
// Spacing rule (exact master parity):
//   pending_space is committed only when: !cjk_text OR
//     (current_token.codepoint < 0x80 AND last_output_byte < 0x80)
//
static std::string LegacyCoreNormalize(const std::string& text, bool cjk_text) {
  const auto tokens = Tokenize(text);
  std::string out;
  bool pending_space = false;

  for (size_t i = 0; i < tokens.size(); ) {
    const auto& tok = tokens[i];

    if (tok.cp < 0x80 && IsAsciiSpace(static_cast<char>(tok.cp))) {
      pending_space = true;
      ++i;
      continue;
    }

    // Number-dot: digit . digit — kept as-is
    bool is_number_dot = false;
    if (tok.text == ".") {
      bool prev_digit = !out.empty() && IsAsciiDigit(out.back());
      bool next_digit = false;
      for (size_t k = i + 1; k < tokens.size(); ++k) {
        if (tokens[k].cp < 0x80 && IsAsciiSpace(static_cast<char>(tokens[k].cp))) continue;
        next_digit = IsDigitStr(tokens[k].text);
        break;
      }
      is_number_dot = prev_digit && next_digit;
    }

    std::string norm = is_number_dot ? "." : NormPunct(tok.text, cjk_text);

    if (IsAnyPunct(norm)) {
      out = TrimRight(std::move(out));
      out += norm;
      pending_space = false;

      bool saw_space_after = false;
      size_t j = i + 1;
      while (j < tokens.size()) {
        const auto& nxt = tokens[j];
        if (nxt.cp < 0x80 && IsAsciiSpace(static_cast<char>(nxt.cp))) {
          saw_space_after = true;
          ++j;
          continue;
        }
        if (!IsAnyPunct(NormPunct(nxt.text, cjk_text))) break;
        ++j;
      }
      pending_space = !cjk_text && saw_space_after;
      i = j;
      continue;
    }

    // Master spacing rule: add pending space only if !cjk_text, or both
    // sides are ASCII bytes.
    if (pending_space && !out.empty()) {
      if (!cjk_text || (tok.cp < 0x80 &&
                        static_cast<unsigned char>(out.back()) < 0x80)) {
        out += ' ';
      }
    }
    out += norm;
    pending_space = false;
    ++i;
  }

  return TrimRight(std::move(out));
}

// NormalizeWithProtection: apply LegacyCoreNormalize (exact master parity) while
// preserving the byte content of protected spans verbatim.
//
// Algorithm:
//   1. cjk_text is determined from the ORIGINAL text so that CJK chars inside
//      protected spans still influence the punctuation mode for the utterance.
//   2. Each protected span is replaced with a collision-free all-ASCII
//      placeholder (no punctuation chars) so the legacy core does not mangle
//      the protected content.
//   3. LegacyCoreNormalize runs on the substituted string.
//   4. Each placeholder is replaced back with its original protected content.
//
static std::string NormalizeWithProtection(
    const std::string& text,
    const std::vector<ProtectedSpan>& spans) {

  // cjk_text is always derived from the original input.
  const bool cjk_text = ContainsFullWidthPunctChar(Tokenize(text));

  if (spans.empty()) {
    return LegacyCoreNormalize(text, cjk_text);
  }

  const size_t n_spans = spans.size();

  // Generate collision-free placeholders containing only letters and digits.
  // Explicit separators prevent one span id from being a prefix of another
  // (for example, id 1 versus id 10), so restoration order cannot matter.
  std::vector<std::string> placeholders(n_spans);
  for (size_t i = 0; i < n_spans; ++i) {
    for (size_t attempt = 0; ; ++attempt) {
      const std::string candidate =
          "VTPROTECTEDTOKEN" + std::to_string(i) + "X" +
          std::to_string(attempt) + "END";
      if (text.find(candidate) == std::string::npos) {
        placeholders[i] = candidate;
        break;
      }
    }
  }

  // Build substituted string (right-to-left to keep byte offsets valid).
  std::string substituted = text;
  for (size_t i = n_spans; i-- > 0; ) {
    const auto& sp = spans[i];
    substituted = substituted.substr(0, sp.begin)
                + placeholders[i]
                + substituted.substr(sp.end);
  }

  // Run legacy core on substituted text with cjk_text from original.
  std::string result = LegacyCoreNormalize(substituted, cjk_text);

  // Restore each placeholder to its original protected content.
  for (size_t i = 0; i < n_spans; ++i) {
    const std::string& ph = placeholders[i];
    const std::string orig_content(text.data() + spans[i].begin,
                                   spans[i].end - spans[i].begin);
    size_t pos = 0;
    while ((pos = result.find(ph, pos)) != std::string::npos) {
      result.replace(pos, ph.size(), orig_content);
      pos += orig_content.size();
    }
  }

  return result;
}

// ─── Filler removal ───────────────────────────────────────────────────────────

// Returns true if position `pos` in `text` is at sentence start or just after
// a punctuation character (fullwidth or halfwidth).
static bool AtFillerBoundary(const std::string& text, size_t pos) {
  if (pos == 0) return true;
  // Walk backwards skipping spaces
  size_t j = pos;
  while (j > 0 && IsAsciiSpace(text[j - 1])) --j;
  if (j == 0) return true;

  // Find the start of the last UTF-8 character before j.
  // UTF-8 continuation bytes have the form 10xxxxxx (0x80..0xBF).
  // Walk backwards to find the leading byte.
  size_t char_start = j - 1;
  while (char_start > 0 &&
         (static_cast<unsigned char>(text[char_start]) & 0xC0) == 0x80) {
    --char_start;
  }
  const size_t remaining = j - char_start;
  Utf8Char c = DecodeUtf8(text.data() + char_start, remaining);
  if (c.byte_len == 0) return false;
  const std::string tok(c.bytes, c.byte_len);
  return IsAnyPunct(tok);
}

// Remove Chinese fillers (嗯、呃) at sentence start or after punctuation boundary.
static std::string RemoveChineseFillers(const std::string& text,
                                        const std::vector<std::string>& fillers,
                                        const std::vector<ProtectedSpan>& spans) {
  std::string out = text;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& filler : fillers) {
      size_t pos = 0;
      std::string next = out;
      while ((pos = next.find(filler, pos)) != std::string::npos) {
        // Not in protected span
        bool protected_here = false;
        for (const auto& s : spans) {
          if (pos >= s.begin && pos < s.end) { protected_here = true; break; }
        }
        if (protected_here) { pos += filler.size(); continue; }

        if (AtFillerBoundary(next, pos)) {
          size_t end = pos + filler.size();
          // Also eat a trailing comma (full-width or half-width) that belongs
          // to the filler, e.g. "嗯，" or "嗯," at the start of an utterance.
          // Full-width comma "，" is 3 bytes (UTF-8: E3 80 81... wait, actually
          // "，" is U+FF0C = EF BC 8C).
          // Check for fullwidth comma ， or enumeration comma 、
          if (end + 3 <= next.size() &&
              ((static_cast<unsigned char>(next[end]) == 0xEF &&
                static_cast<unsigned char>(next[end+1]) == 0xBC &&
                static_cast<unsigned char>(next[end+2]) == 0x8C) ||
               (static_cast<unsigned char>(next[end]) == 0xE3 &&
                static_cast<unsigned char>(next[end+1]) == 0x80 &&
                static_cast<unsigned char>(next[end+2]) == 0x81))) {
            end += 3;
          } else if (end < next.size() && next[end] == ',') {
            end += 1;
          }
          // Eat following ASCII spaces
          while (end < next.size() && next[end] == ' ') ++end;
          next = next.substr(0, pos) + next.substr(end);
          changed = true;
          // Restart scan from same pos
        } else {
          pos += filler.size();
        }
      }
      out = next;
    }
  }
  return out;
}

// Remove English fillers (um, uh) at sentence start or after punctuation.
// Must match as whole words (not inside album, thunder etc.)
static std::string RemoveEnglishFillers(const std::string& text,
                                        const std::vector<std::string>& fillers,
                                        const std::vector<ProtectedSpan>& spans) {
  std::string out = text;
  for (const auto& filler : fillers) {
    const std::string flo = ToLower(filler);
    std::string result;
    size_t i = 0;
    while (i < out.size()) {
      // Case-insensitive match
      if (i + flo.size() <= out.size() &&
          ToLower(out.substr(i, flo.size())) == flo) {
        // Check word boundary after
        size_t end = i + flo.size();
        bool word_end = (end >= out.size()) || IsWordBoundary(out[end]);
        bool at_boundary = AtFillerBoundary(out, i);

        // Not in protected span
        bool in_span = false;
        for (const auto& s : spans) {
          if (i >= s.begin && i < s.end) { in_span = true; break; }
        }

        if (at_boundary && word_end && !in_span) {
          // Skip filler and any following space
          i = end;
          while (i < out.size() && out[i] == ' ') ++i;
          // Don't add to result; also handle start of string
          // If result ends with ", " and we removed filler, trim trailing space
          if (!result.empty() && result.back() == ' ') {
            // keep the comma, remove trailing space if next token follows
          }
          continue;
        }
      }
      result += out[i];
      ++i;
    }
    out = result;
  }
  return out;
}

// ─── Repeat removal ───────────────────────────────────────────────────────────

// Single-character CJK reduplication patterns that should be preserved.
// e.g., 人人, 天天, 常常, 看看
static bool IsIntentionalReduplication(const std::string& phrase) {
  // Decode the phrase as UTF-8; if it is exactly 2 CJK codepoints that are identical,
  // it's intentional reduplication (single char repeated).
  const auto tokens = Tokenize(phrase);
  if (tokens.size() == 2 && tokens[0].text == tokens[1].text &&
      IsCjkAny(tokens[0].cp)) {
    return true;
  }
  return false;
}

// Default CJK 2-char phrase allowlist: only these are de-duplicated.
// These are common ASR false-repeats. Longer phrases (3+ chars) are always
// eligible for de-dup since they are extremely unlikely to be intentional.
static const std::vector<std::string> kDefaultCjkRepeatAllowlist = {
    "今天", "然后", "会议", "问题", "这个", "那个",
    "确认", "开始", "继续", "结束", "一下", "好的",
    "知道", "可以", "需要", "时候", "准备", "完成",
};

// Default English function-word allowlist for repeat removal.
static const std::vector<std::string> kDefaultEnglishRepeatAllowlist = {
    "the", "a", "an", "is", "are", "was", "were",
    "to", "of", "in", "and", "or", "but",
    "i", "it", "he", "she", "we", "they",
    "do", "does", "did", "be", "been",
    "at", "on", "for", "with", "by",
};

// Remove adjacent identical CJK phrases of >= min_chars codepoints.
// For 2-char phrases, only allowlisted phrases are de-duplicated (high-precision).
// For 3+ char phrases, all are eligible (very unlikely to be intentional).
static std::string RemoveCjkPhraseRepeats(const std::string& text,
                                          int min_chars,
                                          const std::vector<std::string>& cjk_allowlist,
                                          const std::vector<ProtectedSpan>& spans) {
  std::string current = text;
  // Iterate until stable (each pass removes at most one set of repeats per position)
  for (int pass = 0; pass < 20; ++pass) {
    const auto tokens = Tokenize(current);
    const int n = static_cast<int>(tokens.size());
    std::string out;
    bool changed = false;
    int i = 0;

    while (i < n) {
      // Check if token is in protected span
      bool in_span = false;
      for (const auto& s : spans) {
        if (tokens[i].byte_pos >= s.begin && tokens[i].byte_pos < s.end) {
          in_span = true;
          break;
        }
      }
      if (in_span || !IsCjkAny(tokens[i].cp)) {
        out += tokens[i].text;
        ++i;
        continue;
      }

      // Try longest phrase first (cap at 20 codepoints)
      bool found_repeat = false;
      int max_k = std::min(n - i, 20);
      for (int k = max_k / 2; k >= min_chars; --k) {
        if (i + 2 * k > n) continue;

        // Build phrase of k codepoints starting at i
        bool phrase_has_protected = false;
        std::string phrase, next_phrase;
        for (int p = i; p < i + k; ++p) {
          phrase += tokens[p].text;
          for (const auto& s : spans) {
            if (tokens[p].byte_pos >= s.begin && tokens[p].byte_pos < s.end)
              phrase_has_protected = true;
          }
        }
        if (phrase_has_protected) break;

        bool next_has_protected = false;
        for (int p = i + k; p < i + 2 * k; ++p) {
          next_phrase += tokens[p].text;
          for (const auto& s : spans) {
            if (tokens[p].byte_pos >= s.begin && tokens[p].byte_pos < s.end)
              next_has_protected = true;
          }
        }
        if (next_has_protected) continue;

        if (phrase != next_phrase) continue;

        // Single-char: skip intentional reduplication (e.g. 人人, 天天)
        if (k == 1 && IsIntentionalReduplication(phrase + phrase)) continue;

        // All phrase lengths (2+ chars): only de-dup if phrase is in the allowlist.
        // Explicit allowlist is the single source of truth; no implicit generalization
        // for longer phrases.
        {
          bool in_allowlist = false;
          for (const auto& allowed : cjk_allowlist) {
            if (phrase == allowed) { in_allowlist = true; break; }
          }
          if (!in_allowlist) continue;
        }

        // Emit the first copy, skip the second copy, advance past both
        out += phrase;
        i += 2 * k;  // skip both; we already emitted first copy
        found_repeat = true;
        changed = true;
        break;
      }

      if (!found_repeat) {
        out += tokens[i].text;
        ++i;
      }
    }

    current = out;
    if (!changed) break;
  }
  return current;
}

// Remove adjacent identical English words (standalone, word boundary).
// Only function words listed in english_allowlist are de-duplicated.
static std::string RemoveEnglishWordRepeats(const std::string& text,
                                            const std::vector<std::string>& english_allowlist,
                                            const std::vector<ProtectedSpan>& spans) {
  // Tokenize into words and non-word runs, find adjacent identical words.
  struct Chunk {
    std::string text;
    bool is_word;    // true = sequence of letters
    size_t byte_pos; // start byte offset in the working string
  };

  auto Chunk_from = [](const std::string& t, bool iw, size_t p) -> Chunk {
    return {t, iw, p};
  };

  // Rebuild string in chunks
  std::vector<Chunk> chunks;
  size_t i = 0;
  while (i < text.size()) {
    if (IsAsciiLetter(text[i])) {
      size_t start = i;
      while (i < text.size() && IsAsciiLetter(text[i])) ++i;
      chunks.push_back(Chunk_from(text.substr(start, i - start), true, start));
    } else {
      chunks.push_back(Chunk_from(std::string(1, text[i]), false, i));
      ++i;
    }
  }

  // Find adjacent identical words separated only by spaces
  std::vector<bool> removed(chunks.size(), false);
  for (size_t c = 0; c + 1 < chunks.size(); ++c) {
    if (!chunks[c].is_word || removed[c]) continue;
    // Find next word chunk, allowing only spaces in between
    size_t gap_start = c + 1;
    size_t next_word = std::string::npos;
    bool only_spaces = true;
    for (size_t d = gap_start; d < chunks.size(); ++d) {
      if (chunks[d].is_word) { next_word = d; break; }
      for (char ch : chunks[d].text) {
        if (!IsAsciiSpace(ch)) { only_spaces = false; break; }
      }
      if (!only_spaces) break;
    }
    if (next_word == std::string::npos || !only_spaces) continue;
    if (ToLower(chunks[c].text) != ToLower(chunks[next_word].text)) continue;

    // Only de-dup if word is in the allowlist (function words only)
    {
      bool in_allowlist = false;
      const std::string wl = ToLower(chunks[c].text);
      for (const auto& allowed : english_allowlist) {
        if (wl == allowed) { in_allowlist = true; break; }
      }
      if (!in_allowlist) continue;
    }

    // Check neither is in a protected span
    bool protected_c = false, protected_n = false;
    for (const auto& s : spans) {
      if (chunks[c].byte_pos >= s.begin && chunks[c].byte_pos < s.end) protected_c = true;
      if (chunks[next_word].byte_pos >= s.begin && chunks[next_word].byte_pos < s.end) protected_n = true;
    }
    if (protected_c || protected_n) continue;

    // Remove the second occurrence and the gap spaces before it
    removed[next_word] = true;
    // Also remove the spaces between c and next_word
    for (size_t d = gap_start; d < next_word; ++d) removed[d] = true;
    // But keep one space after the first word if something follows
    // (add it back after the first word)
  }

  // Rebuild
  std::string out;
  for (size_t c = 0; c < chunks.size(); ++c) {
    if (removed[c]) continue;
    out += chunks[c].text;
  }
  // If a word was deduplicated and nothing precedes the next token, we may
  // need to ensure one space. Let's just normalize multiple spaces to one.
  // (Simple pass: collapse double-spaces)
  std::string result;
  bool last_space = false;
  for (char ch : out) {
    if (ch == ' ') {
      if (!last_space) result += ch;
      last_space = true;
    } else {
      result += ch;
      last_space = false;
    }
  }
  return TrimRight(result);
}

// ─── Self-repair removal ──────────────────────────────────────────────────────
//
// Pattern: <term>，不对，<replacement> or <term>, 不对, <replacement>
// High-confidence patterns: Chinese weekday, number word, version number.

static const std::vector<std::string> kChineseWeekdays = {
    "周一", "周二", "周三", "周四", "周五", "周六", "周日", "周天",
    "星期一", "星期二", "星期三", "星期四", "星期五", "星期六", "星期日"};

static bool IsChineseWeekday(const std::string& s) {
  return std::find(kChineseWeekdays.begin(), kChineseWeekdays.end(), s) !=
         kChineseWeekdays.end();
}

// Chinese number words (individual digit/unit characters)
static const std::vector<std::string> kChineseNumbers = {
    "零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十",
    "百", "千", "万", "亿", "两"};

// Chinese clock times: 三点, 四点, 十二点半 etc.
static bool IsChineseClockTime(const std::string& s) {
  const auto toks = Tokenize(s);
  if (toks.empty()) return false;
  // Last token should be 点/分/半 or end after digit
  const std::string last = toks.back().text;
  const bool ends_time = (last == "点" || last == "分" || last == "半" ||
                          std::find(kChineseNumbers.begin(), kChineseNumbers.end(), last)
                          != kChineseNumbers.end());
  if (!ends_time) return false;
  // All tokens before last should be number chars or time-unit chars
  for (int j = 0; j < (int)toks.size() - 1; ++j) {
    const std::string& t = toks[j].text;
    bool ok = (std::find(kChineseNumbers.begin(), kChineseNumbers.end(), t)
               != kChineseNumbers.end()) ||
              t == "点" || t == "分" || t == "半";
    if (!ok) return false;
  }
  return true;
}

static bool IsChineseNumberWord(const std::string& s) {
  const auto tokens = Tokenize(s);
  for (const auto& t : tokens) {
    if (std::find(kChineseNumbers.begin(), kChineseNumbers.end(), t.text) ==
        kChineseNumbers.end()) {
      return false;
    }
  }
  return !s.empty();
}

// Version-like: N.N[.N]*
static bool IsVersionLike(const std::string& s) {
  bool has_dot = false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (IsAsciiDigit(s[i])) continue;
    if (s[i] == '.' && i + 1 < s.size() && IsAsciiDigit(s[i + 1])) {
      has_dot = true;
      continue;
    }
    return false;
  }
  return has_dot;
}

// Remove self-repair patterns of the form: <A>，不对，<B> -> <B>
// where A and B are in the same high-confidence category.
static std::string RemoveSelfRepair(const std::string& text,
                                    const std::vector<ProtectedSpan>& spans) {
  // Correction markers (Chinese: 不对/不是)
  // Pattern: [boundary][A][marker][B][boundary_or_end]
  // We scan for markers, then extract A (going backwards) and B (going forwards).

  static const std::vector<std::string> kMarkers = {
      "，不对，", "，不是，",
      ",不对,",  ",不是,",
      "，不对,",  "，不是,",
      ",不对，",  ",不是，"};

  std::string result = text;

  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto& marker : kMarkers) {
      size_t mpos = result.find(marker);
      if (mpos == std::string::npos) continue;

      // Not in protected span
      bool in_span = false;
      for (const auto& s : spans) {
        if (mpos >= s.begin && mpos < s.end) { in_span = true; break; }
      }
      if (in_span) continue;

      const size_t mend = mpos + marker.size();

      // --- Extract B (what follows the marker) ---
      // Skip spaces
      size_t b_start = mend;
      while (b_start < result.size() && result[b_start] == ' ') ++b_start;

      // Read B tokens until punctuation (except number dots), space, or end
      const auto all_toks = Tokenize(result);
      int b_tok_start = -1;
      for (int t = 0; t < (int)all_toks.size(); ++t) {
        if (all_toks[t].byte_pos >= b_start) { b_tok_start = t; break; }
      }
      if (b_tok_start < 0) continue;

      // Collect candidate B tokens (stop at space/fullpunct; allow '.' between digits)
      std::vector<int> b_cand_toks;
      for (int t = b_tok_start; t < (int)all_toks.size() && (int)b_cand_toks.size() < 8; ++t) {
        const auto& tok = all_toks[t];
        if (tok.cp < 0x80) {
          if (IsAsciiSpace(static_cast<char>(tok.cp))) break;
          // Allow '.' between digits (version/IP separator)
          if (tok.text == ".") {
            bool prev_digit = !b_cand_toks.empty() &&
                              IsDigitStr(all_toks[b_cand_toks.back()].text);
            bool next_digit = (t + 1 < (int)all_toks.size()) &&
                              IsDigitStr(all_toks[t + 1].text);
            if (prev_digit && next_digit) { b_cand_toks.push_back(t); continue; }
          }
          if (IsHalfPunct(tok.text)) break;
        }
        if (IsFullPunct(tok.text)) break;
        b_cand_toks.push_back(t);
      }
      if (b_cand_toks.empty()) continue;

      // Try longest-to-shortest B prefix for category match
      std::string b_text;
      size_t b_byte_end = 0;

      // Build prefixes from longest to shortest
      for (int plen = (int)b_cand_toks.size(); plen >= 1; --plen) {
        std::string cand;
        for (int j = 0; j < plen; ++j) cand += all_toks[b_cand_toks[j]].text;
        bool cat_ok = (IsChineseWeekday(cand) || IsChineseNumberWord(cand) ||
                       IsChineseClockTime(cand) || IsVersionLike(cand));
        if (cat_ok) {
          b_text = cand;
          int last_tok = b_cand_toks[plen - 1];
          b_byte_end = all_toks[last_tok].byte_pos + all_toks[last_tok].text.size();
          break;
        }
      }
      if (b_text.empty()) continue;

      // --- Extract A (what precedes the marker) ---
      // Find tokens before the marker
      const auto all_toks_a = Tokenize(result);  // re-tokenize on current result
      int a_tok_end_idx = -1;
      for (int t = (int)all_toks_a.size() - 1; t >= 0; --t) {
        if (all_toks_a[t].byte_pos + all_toks_a[t].text.size() <= mpos) {
          a_tok_end_idx = t;
          break;
        }
      }
      if (a_tok_end_idx < 0) continue;

      // Collect candidate A tokens (going backwards, max 8)
      // Allow '.' between digits
      std::vector<int> a_cand_toks;
      for (int t = a_tok_end_idx; t >= 0 && (int)a_cand_toks.size() < 8; --t) {
        const auto& tok = all_toks_a[t];
        if (tok.cp < 0x80) {
          if (IsAsciiSpace(static_cast<char>(tok.cp))) break;
          // Allow '.' between digits (version separator)
          if (tok.text == ".") {
            bool next_digit = !a_cand_toks.empty() &&
                              IsDigitStr(all_toks_a[a_cand_toks.back()].text);
            bool prev_digit = (t - 1 >= 0) && IsDigitStr(all_toks_a[t - 1].text);
            if (prev_digit && next_digit) { a_cand_toks.push_back(t); continue; }
          }
          if (IsHalfPunct(tok.text)) break;
        }
        if (IsFullPunct(tok.text)) break;
        a_cand_toks.push_back(t);
      }
      if (a_cand_toks.empty()) continue;
      // Reverse so a_cand_toks is in forward order
      std::reverse(a_cand_toks.begin(), a_cand_toks.end());

      // Try longest-to-shortest A suffix for category match
      std::string a_text;
      size_t a_byte_start = 0;
      for (int plen = (int)a_cand_toks.size(); plen >= 1; --plen) {
        // Take last plen tokens from a_cand_toks
        int first_a = a_cand_toks[(int)a_cand_toks.size() - plen];
        std::string cand;
        for (int j = (int)a_cand_toks.size() - plen; j < (int)a_cand_toks.size(); ++j)
          cand += all_toks_a[a_cand_toks[j]].text;
        bool cat_ok = (IsChineseWeekday(cand) || IsChineseNumberWord(cand) ||
                       IsChineseClockTime(cand) || IsVersionLike(cand));
        if (cat_ok) {
          a_text = cand;
          a_byte_start = all_toks_a[first_a].byte_pos;
          break;
        }
      }
      if (a_text.empty()) continue;

      // --- Check same high-confidence category ---
      bool same_cat = false;
      if (IsChineseWeekday(a_text) && IsChineseWeekday(b_text)) same_cat = true;
      if (IsChineseNumberWord(a_text) && IsChineseNumberWord(b_text)) same_cat = true;
      if (IsChineseClockTime(a_text) && IsChineseClockTime(b_text)) same_cat = true;
      if (IsVersionLike(a_text) && IsVersionLike(b_text)) same_cat = true;

      if (!same_cat) continue;

      // Replace [a_byte_start, b_byte_end) with b_text
      result = result.substr(0, a_byte_start) + b_text + result.substr(b_byte_end);
      changed = true;
      break;  // re-scan from scratch
    }
  }

  return result;
}

// ─── Alias correction ─────────────────────────────────────────────────────────

// Apply alias correction to text, respecting protected spans.
// Uses longest-match-first strategy.
static std::string ApplyAliases(const std::string& text,
                                 const std::vector<AliasEntry>& entries,
                                 const ProtectedSpanConfig& span_cfg) {
  if (entries.empty()) return text;

  // Build sorted list of (alias_lower, canonical) pairs, longest first.
  // Deduplicate: if the same alias string appears for multiple canonicals,
  // the first occurrence (user entries prepend before builtins) wins.
  struct AliasMatch {
    std::string alias_lower;
    std::string canonical;
    bool is_cjk;
  };
  std::vector<AliasMatch> matches;
  std::unordered_set<std::string> seen_aliases;
  for (const auto& entry : entries) {
    for (const auto& alias : entry.aliases) {
      if (alias.empty()) continue;
      const std::string al = ToLower(alias);
      if (seen_aliases.count(al)) continue;  // earlier entry (user) wins
      seen_aliases.insert(al);
      bool cjk = false;
      const auto toks = Tokenize(alias);
      for (const auto& t : toks) {
        if (IsCjkAny(t.cp)) { cjk = true; break; }
      }
      matches.push_back({al, entry.canonical, cjk});
    }
  }
  // Sort longest first so "json rpc" beats "json".
  std::sort(matches.begin(), matches.end(),
            [](const AliasMatch& a, const AliasMatch& b) {
              return a.alias_lower.size() > b.alias_lower.size();
            });

  std::string result = text;

  for (const auto& m : matches) {
    const std::string& alias = m.alias_lower;
    const std::string& canon = m.canonical;
    const size_t alen = alias.size();

    // Re-detect protected spans on the CURRENT result so byte offsets are correct
    // even after previous alias replacements mutated the string.
    const auto cur_spans = FindProtectedSpansImpl(span_cfg, result);

    auto InSpanNow = [&](size_t pos, size_t len) -> bool {
      for (const auto& s : cur_spans) {
        // Overlap: match window [pos, pos+len) overlaps span [s.begin, s.end)
        if (pos < s.end && pos + len > s.begin) return true;
      }
      return false;
    };

    std::string out;
    size_t i = 0;
    while (i < result.size()) {
      if (i + alen > result.size()) { out += result.substr(i); break; }

      // Case-insensitive compare
      if (ToLower(result.substr(i, alen)) != alias) { out += result[i]; ++i; continue; }

      // Word boundary for Latin aliases
      if (!m.is_cjk) {
        bool left_ok  = (i == 0) || IsWordBoundary(result[i - 1]);
        bool right_ok = (i + alen >= result.size()) || IsWordBoundary(result[i + alen]);
        if (!left_ok || !right_ok) { out += result[i]; ++i; continue; }
      }

      // Protected span check on current result text
      if (InSpanNow(i, alen)) { out += result[i]; ++i; continue; }

      // Replace
      out += canon;
      i += alen;
    }
    result = out;
  }

  return result;
}

// ─── Config loading ───────────────────────────────────────────────────────────

static std::vector<AliasEntry> LoadBuiltinAliases() {
  // Hard-coded defaults matching data/computer_terms.json
  return {
      {"Vibetype",   {"vibetype"}},
      {"JSON-RPC",   {"jsonrpc", "json rpc", "json-rpc"}},
      {"GitHub",     {"github", "Github", "git hub"}},
      {"GitLab",     {"gitlab", "Gitlab", "git lab"}},
      {"Kubernetes", {"kubernetes", "k8s", "K8S"}},
      {"JavaScript", {"javascript", "Javascript", "java script"}},
      {"TypeScript", {"typescript", "Typescript", "type script"}},
      {"WebSocket",  {"websocket", "Websocket", "web socket"}},
      {"ChatGPT",    {"chatgpt", "chat gpt", "Chat GPT"}},
      {"OpenAI",     {"openai", "open ai", "Open AI"}},
      {"YouTube",    {"youtube", "You Tube", "you tube"}},
      {"WiFi",       {"wifi", "wi fi", "wi-fi", "Wifi"}},
      {"macOS",      {"macos", "MacOS", "Mac OS", "mac os"}},
      {"iPhone",     {"iphone", "IPhone", "i phone"}},
      {"Linux",      {"linux"}},
      {"ALSA",       {"alsa"}},
      {"IBus",       {"ibus", "Ibus", "I Bus", "i bus"}},
      {"Fcitx5",     {"fcitx5", "Fcitx 5", "fcitx 5"}},
      {"SenseVoice", {"sensevoice", "Sense Voice", "sense voice"}},
  };
}

// ─── Config loading: alias JSON (xtils::Json-based) ──────────────────────────

// Parse alias entries from a JSON object (xtils::Json).
// Format: {"aliases": [{"canonical": "X", "aliases": ["a", "b"]}, ...]}
// If the "aliases" key is absent or invalid, returns an empty vector.
static std::vector<AliasEntry> ParseAliasJson(const xtils::Json& doc) {
  std::vector<AliasEntry> entries;
  if (!doc.is_object()) return entries;
  const xtils::Json* arr_ptr = doc.find("aliases");
  if (!arr_ptr || !arr_ptr->is_array()) return entries;
  for (const auto& item : arr_ptr->as_array()) {
    if (!item.is_object()) continue;
    const xtils::Json* canon_ptr = item.find("canonical");
    const xtils::Json* al_ptr   = item.find("aliases");
    if (!canon_ptr || !canon_ptr->is_string()) continue;
    AliasEntry entry;
    entry.canonical = canon_ptr->as_string();
    if (al_ptr && al_ptr->is_array()) {
      for (const auto& a : al_ptr->as_array()) {
        if (a.is_string() && !a.as_string().empty())
          entry.aliases.push_back(a.as_string());
      }
    }
    if (!entry.canonical.empty()) entries.push_back(std::move(entry));
  }
  return entries;
}

// Apply recognized fields from a parsed text-processing JSON object onto cfg.
// Only explicitly present fields are modified (conservative layering).
static void ApplyTextProcessingJson(const xtils::Json& doc, TextProcessorConfig& cfg) {
  if (!doc.is_object()) return;

  auto ApplyBoolField = [&](const std::string& section, const std::string& key,
                             bool& target) {
    const xtils::Json* sec = doc.find(section);
    if (!sec || !sec->is_object()) return;
    const xtils::Json* val = sec->find(key);
    if (val && val->is_bool()) target = val->as_bool();
  };

  ApplyBoolField("filler_removal",   "enabled",           cfg.filler.enabled);
  ApplyBoolField("repeat_removal",   "enabled",           cfg.repeat.enabled);
  ApplyBoolField("self_repair",      "enabled",           cfg.self_repair.enabled);
  ApplyBoolField("alias_correction", "enabled",           cfg.alias.enabled);
  ApplyBoolField("protected_spans",  "url",               cfg.protected_spans.url);
  ApplyBoolField("protected_spans",  "email",             cfg.protected_spans.email);
  ApplyBoolField("protected_spans",  "path",              cfg.protected_spans.path);
  ApplyBoolField("protected_spans",  "cli_option",        cfg.protected_spans.cli_option);
  ApplyBoolField("protected_spans",  "version",           cfg.protected_spans.version);
  ApplyBoolField("protected_spans",  "backtick_code",     cfg.protected_spans.backtick_code);
  ApplyBoolField("protected_spans",  "tech_identifier",   cfg.protected_spans.tech_identifier);

  // repeat_removal.min_phrase_chars
  {
    const xtils::Json* sec = doc.find("repeat_removal");
    if (sec && sec->is_object()) {
      const xtils::Json* val = sec->find("min_phrase_chars");
      if (val && val->is_integer() && val->as_integer() > 0)
        cfg.repeat.min_phrase_chars = static_cast<int>(val->as_integer());
    }
  }

  // filler_removal.chinese_fillers / english_fillers
  auto ApplyStringArray = [&](const std::string& section, const std::string& key,
                               std::vector<std::string>& target) {
    const xtils::Json* sec = doc.find(section);
    if (!sec || !sec->is_object()) return;
    const xtils::Json* arr = sec->find(key);
    if (!arr || !arr->is_array()) return;
    // Key present (even if empty array): always replace
    target.clear();
    for (const auto& item : arr->as_array()) {
      if (item.is_string()) target.push_back(item.as_string());
    }
  };
  ApplyStringArray("filler_removal", "chinese_fillers", cfg.filler.chinese_fillers);
  ApplyStringArray("filler_removal", "english_fillers",  cfg.filler.english_fillers);
  ApplyStringArray("repeat_removal", "cjk_allowlist",   cfg.repeat.cjk_allowlist);
  ApplyStringArray("repeat_removal", "english_allowlist", cfg.repeat.english_allowlist);
}


// ─── TextProcessorConfig ─────────────────────────────────────────────────────

TextProcessorConfig TextProcessorConfig::Defaults() {
  TextProcessorConfig cfg;
  cfg.alias.entries = LoadBuiltinAliases();
  cfg.repeat.cjk_allowlist = kDefaultCjkRepeatAllowlist;
  cfg.repeat.english_allowlist = kDefaultEnglishRepeatAllowlist;
  return cfg;
}

static std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string ExpandHome(const std::string& path) {
  if (path.empty() || path[0] != '~') return path;
  const char* home = std::getenv("HOME");
  if (!home) return path;
  return std::string(home) + path.substr(1);
}

TextProcessorConfig TextProcessorConfig::Load(const std::string& builtin_data_dir,
                                               const std::string& user_config_path) {
  TextProcessorConfig cfg = Defaults();

  // Helper: read and parse a JSON file.
  // Returns nullopt if file is missing/empty; sets *parse_error=true if the file
  // exists but contains invalid JSON or a non-object root.
  auto ReadJsonFile = [](const std::string& path,
                         bool* parse_error) -> std::optional<xtils::Json> {
    if (parse_error) *parse_error = false;
    const std::string content = ReadFile(path);
    if (content.empty()) return std::nullopt;
    auto doc = xtils::Json::parse(content);
    if (!doc || !doc->is_object()) {
      if (parse_error) *parse_error = true;
      return std::nullopt;
    }
    return doc;
  };

  // ── Step 1: Load builtin text-processing.json (switches) ──────────────────
  if (!builtin_data_dir.empty()) {
    const std::string tp_path = builtin_data_dir + "/text-processing.json";
    bool tp_err = false;
    if (auto doc = ReadJsonFile(tp_path, &tp_err)) {
      ApplyTextProcessingJson(*doc, cfg);
      TP_LOGI("loaded text-processing config from %s", tp_path.c_str());
    } else if (tp_err) {
      TP_LOGW("invalid JSON or wrong root type in %s — using defaults",
              tp_path.c_str());
      // Whole-file fallback: cfg already holds Defaults(), nothing to undo.
    }
    // else: file not found/empty, silently use defaults

    // ── Step 2: Load builtin computer_terms.json (alias dictionary) ──────────
    const std::string terms_path = builtin_data_dir + "/computer_terms.json";
    bool terms_err = false;
    if (auto doc = ReadJsonFile(terms_path, &terms_err)) {
      auto file_entries = ParseAliasJson(*doc);
      if (!file_entries.empty()) {
        cfg.alias.entries = std::move(file_entries);
        TP_LOGI("loaded %zu alias entries from %s",
                cfg.alias.entries.size(), terms_path.c_str());
      } else {
        TP_LOGW("no alias entries in %s, using builtin defaults",
                terms_path.c_str());
      }
    } else if (terms_err) {
      TP_LOGW("invalid JSON or wrong root type in %s — using builtin alias defaults",
              terms_path.c_str());
      // Whole-file fallback: cfg.alias.entries already holds LoadBuiltinAliases().
    }
    // else: file not found/empty, silently use defaults
  }

  // ── Step 3: Load user config override ────────────────────────────────────
  std::string user_path = user_config_path;
  if (user_path.empty()) {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
      user_path = std::string(xdg) + "/vibetype/text-processing.json";
    } else {
      const char* home = std::getenv("HOME");
      if (home && *home) {
        user_path = std::string(home) + "/.config/vibetype/text-processing.json";
      }
    }
  }
  user_path = ExpandHome(user_path);

  if (!user_path.empty()) {
    const std::string user_content = ReadFile(user_path);
    if (!user_content.empty()) {
      auto user_doc = xtils::Json::parse(user_content);
      if (!user_doc || !user_doc->is_object()) {
        TP_LOGE("invalid JSON in user config %s — ignoring entire file",
                user_path.c_str());
      } else {
        // Apply switches (conservative: only explicitly present fields override)
        ApplyTextProcessingJson(*user_doc, cfg);
        TP_LOGI("applied user config overrides from %s", user_path.c_str());

        // Merge user alias entries based on presence of "aliases" key:
        // - key absent: keep builtin entries unchanged
        // - key present, empty array: clear all entries (user explicitly wants none)
        // - key present, non-empty: user entries prepend (shadowing builtins), deduplicate
        const xtils::Json* aliases_key = user_doc->find("aliases");
        if (aliases_key != nullptr) {
          if (!aliases_key->is_array()) {
            TP_LOGW("user config %s: 'aliases' is not an array — ignoring aliases key",
                    user_path.c_str());
          } else {
            auto user_entries = ParseAliasJson(*user_doc);
            if (user_entries.empty()) {
              // Explicit []: clear all entries
              cfg.alias.entries.clear();
              TP_LOGI("user config %s: explicit empty aliases — cleared all entries",
                      user_path.c_str());
            } else {
              // user entries first; append builtin entries that don't conflict
              // (same canonical already covered by a user entry)
              std::unordered_set<std::string> user_canonicals;
              for (const auto& ue : user_entries) {
                user_canonicals.insert(ue.canonical);
              }
              std::vector<AliasEntry> merged = user_entries;
              for (auto& e : cfg.alias.entries) {
                if (user_canonicals.find(e.canonical) == user_canonicals.end())
                  merged.push_back(std::move(e));
              }
              cfg.alias.entries = std::move(merged);
              TP_LOGI("merged %zu user alias entries from %s",
                      user_entries.size(), user_path.c_str());
            }
          }
        }
      }
    }
  }

  return cfg;
}

// ─── TextProcessor ────────────────────────────────────────────────────────────

TextProcessor::TextProcessor(TextProcessorConfig cfg) : cfg_(std::move(cfg)) {}

std::string TextProcessor::ProcessPartial(const std::string& text) const {
  // Partial: only safe normalization (punctuation + space).
  // Detect protected spans first so normalization doesn't break them.
  const auto spans = FindProtectedSpansImpl(cfg_.protected_spans, text);
  return NormalizeWithProtection(text, spans);
}

std::string TextProcessor::ProcessFinal(const std::string& text) const {
  // Final pipeline.  Each step re-detects protected spans on the CURRENT
  // result string so byte offsets are always accurate after mutations.
  // 1. Normalize punctuation/spaces
  // 2. Remove fillers
  // 3. Remove repeats
  // 4. Remove self-repair
  // 5. Apply alias correction (re-detects spans per alias pass internally)
  std::string result;
  {
    const auto spans0 = FindProtectedSpansImpl(cfg_.protected_spans, text);
    result = NormalizeWithProtection(text, spans0);
  }

  if (cfg_.filler.enabled) {
    const auto spans1 = FindProtectedSpansImpl(cfg_.protected_spans, result);
    result = RemoveChineseFillers(result, cfg_.filler.chinese_fillers, spans1);
    const auto spans1b = FindProtectedSpansImpl(cfg_.protected_spans, result);
    result = RemoveEnglishFillers(result, cfg_.filler.english_fillers, spans1b);
  }

  if (cfg_.repeat.enabled) {
    const auto spans2 = FindProtectedSpansImpl(cfg_.protected_spans, result);
    result = RemoveCjkPhraseRepeats(result, cfg_.repeat.min_phrase_chars,
                                    cfg_.repeat.cjk_allowlist, spans2);
    const auto spans2b = FindProtectedSpansImpl(cfg_.protected_spans, result);
    result = RemoveEnglishWordRepeats(result, cfg_.repeat.english_allowlist, spans2b);
  }

  if (cfg_.self_repair.enabled) {
    const auto spans3 = FindProtectedSpansImpl(cfg_.protected_spans, result);
    result = RemoveSelfRepair(result, spans3);
  }

  if (cfg_.alias.enabled) {
    // ApplyAliases re-detects spans on each alias pass internally.
    result = ApplyAliases(result, cfg_.alias.entries, cfg_.protected_spans);
  }

  return result;
}

}  // namespace vibetype
