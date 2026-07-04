#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sensevoice_engine.h"

#include "ggml.h"

#include "xtils/config/config.h"
#include "xtils/logging/logger.h"
#include "xtils/net/ipc_channel.h"
#include "xtils/system/signal_handler.h"
#include "xtils/utils/json.h"
#include "xtils/utils/result.h"

namespace {

std::string RuntimeDir() {
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg && *xdg) return xdg;
  return "/tmp";
}

std::string DefaultSocketPath() { return RuntimeDir() + "/vibetype/vibetype.sock"; }

std::string ConfigDir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return std::string(xdg);
  const char* home = std::getenv("HOME");
  if (home && *home) return std::string(home) + "/.config";
  return RuntimeDir();
}

std::string DefaultModelPath() {
  return ConfigDir() + "/vibetype/models/sensevoice-small-q8.gguf";
}

std::string DefaultModelUrl() {
  return "https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF/resolve/main/sensevoice-small-q8.gguf";
}

std::string TrimLogLine(const char* text) {
  if (!text) return "";
  std::string out(text);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

void GgmlLogCallback(ggml_log_level level, const char* text, void*) {
  const std::string msg = TrimLogLine(text);
  if (msg.empty()) return;
  switch (level) {
    case GGML_LOG_LEVEL_DEBUG:
      LogD("ggml: %s", msg.c_str());
      break;
    case GGML_LOG_LEVEL_WARN:
      LogW("ggml: %s", msg.c_str());
      break;
    case GGML_LOG_LEVEL_ERROR:
      LogE("ggml: %s", msg.c_str());
      break;
    case GGML_LOG_LEVEL_INFO:
    case GGML_LOG_LEVEL_CONT:
    case GGML_LOG_LEVEL_NONE:
    default:
      LogI("ggml: %s", msg.c_str());
      break;
  }
}

void InstallGgmlLogger() { ggml_log_set(GgmlLogCallback, nullptr); }

std::string DirName(const std::string& path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

bool EnsureDir(const std::string& path) {
  if (path.empty() || path == "/") return true;
  std::string cur;
  if (path[0] == '/') cur = "/";
  std::stringstream ss(path);
  std::string part;
  while (std::getline(ss, part, '/')) {
    if (part.empty()) continue;
    if (!cur.empty() && cur.back() != '/') cur += "/";
    cur += part;
    if (::mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
  }
  return true;
}

uint16_t ReadLe16(const unsigned char* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

struct WavInfo {
  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  bool has_data = false;
};

std::optional<WavInfo> ReadWavInfo(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "file_not_found";
    return std::nullopt;
  }

  unsigned char riff[12] = {};
  in.read(reinterpret_cast<char*>(riff), sizeof(riff));
  if (in.gcount() != static_cast<std::streamsize>(sizeof(riff)) ||
      std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
    error = "not_a_wav_file";
    return std::nullopt;
  }

  WavInfo info;
  bool has_fmt = false;
  while (in) {
    unsigned char chunk[8] = {};
    in.read(reinterpret_cast<char*>(chunk), sizeof(chunk));
    if (in.gcount() == 0) break;
    if (in.gcount() != static_cast<std::streamsize>(sizeof(chunk))) break;
    const uint32_t size = ReadLe32(chunk + 4);
    const std::string id(reinterpret_cast<char*>(chunk), 4);

    if (id == "fmt ") {
      std::vector<unsigned char> data(size);
      in.read(reinterpret_cast<char*>(data.data()), size);
      if (data.size() < 16 || in.gcount() != static_cast<std::streamsize>(size)) {
        error = "invalid_fmt_chunk";
        return std::nullopt;
      }
      info.audio_format = ReadLe16(data.data());
      info.channels = ReadLe16(data.data() + 2);
      info.sample_rate = ReadLe32(data.data() + 4);
      info.bits_per_sample = ReadLe16(data.data() + 14);
      has_fmt = true;
    } else if (id == "data") {
      info.has_data = size > 0;
      in.seekg(size, std::ios::cur);
    } else {
      in.seekg(size, std::ios::cur);
    }
    if (size % 2 == 1) in.seekg(1, std::ios::cur);
  }

  if (!has_fmt) {
    error = "missing_fmt_chunk";
    return std::nullopt;
  }
  return info;
}

bool IsRuntimeAudioPath(const std::string& path) {
  const std::string allowed = RuntimeDir() + "/vibetype/";
  return path.rfind(allowed, 0) == 0 && path.find("/../") == std::string::npos;
}

bool ValidateAudioSegment(const std::string& path, std::string& code,
                          std::string& message) {
  if (!IsRuntimeAudioPath(path)) {
    code = "access_denied";
    message = "wav_path must be under " + RuntimeDir() + "/vibetype";
    return false;
  }

  std::string wav_error;
  auto info = ReadWavInfo(path, wav_error);
  if (!info) {
    code = wav_error == "file_not_found" ? "file_not_found" : "invalid_audio_format";
    message = "invalid wav file: " + wav_error;
    return false;
  }
  if (info->audio_format != 1 || info->channels != 1 ||
      info->sample_rate != 16000 || info->bits_per_sample != 16 ||
      !info->has_data) {
    code = "invalid_audio_format";
    message = "expected 16 kHz mono PCM16 WAV";
    return false;
  }
  return true;
}

xtils::Json JsonArray(const std::vector<std::string>& values) {
  xtils::Json arr = xtils::Json::array();
  for (const auto& v : values) arr.push_back(v);
  return arr;
}

xtils::Json JsonArray(const std::vector<int>& values) {
  xtils::Json arr = xtils::Json::array();
  for (int v : values) arr.push_back(v);
  return arr;
}

struct Session {
  bool finished = false;
  int expected_count = -1;
  std::map<int, std::string> segment_text;
  bool final_sent = false;
};

std::string FakeTranscribeSegment(int segment_index, const std::string& fake_text) {
  if (!fake_text.empty()) return fake_text;
  return "fake transcript segment " + std::to_string(segment_index);
}

struct Utf8Token {
  std::string text;
  uint32_t codepoint = 0;
};

std::vector<Utf8Token> TokenizeUtf8(const std::string& text) {
  std::vector<Utf8Token> tokens;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t len = 1;
    uint32_t cp = c;
    if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
      len = 2;
      cp = ((c & 0x1F) << 6) |
           (static_cast<unsigned char>(text[i + 1]) & 0x3F);
    } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
      len = 3;
      cp = ((c & 0x0F) << 12) |
           ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 2]) & 0x3F);
    } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
      len = 4;
      cp = ((c & 0x07) << 18) |
           ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 3]) & 0x3F);
    }
    tokens.push_back({text.substr(i, len), cp});
    i += len;
  }
  return tokens;
}

bool IsCjkCodepoint(uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) ||    // CJK Extension A
         (cp >= 0x4E00 && cp <= 0x9FFF) ||    // CJK Unified
         (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK Compatibility
         (cp >= 0x3040 && cp <= 0x30FF) ||    // Hiragana/Katakana
         (cp >= 0xAC00 && cp <= 0xD7AF);      // Hangul
}

bool ContainsCjk(const std::vector<Utf8Token>& tokens) {
  for (const auto& token : tokens) {
    if (IsCjkCodepoint(token.codepoint)) return true;
  }
  return false;
}

bool IsAsciiSpace(const Utf8Token& token) {
  return token.text == " " || token.text == "\t" || token.text == "\n" ||
         token.text == "\r";
}

bool IsDigit(const std::string& s) {
  return s.size() == 1 && s[0] >= '0' && s[0] <= '9';
}

std::string NormalizePunctuationToken(const std::string& token, bool cjk_text) {
  static const std::map<std::string, std::string> to_full = {
      {".", "。"}, {",", "，"}, {"!", "！"}, {"?", "？"},
      {";", "；"}, {":", "："}};
  static const std::map<std::string, std::string> to_half = {
      {"。", "."}, {"，", ","}, {"！", "!"}, {"?", "?"},
      {"；", ";"}, {"：", ":"}, {"、", ","}};
  if (cjk_text) {
    auto it = to_full.find(token);
    return it == to_full.end() ? token : it->second;
  }
  auto it = to_half.find(token);
  return it == to_half.end() ? token : it->second;
}

bool IsPunctuationToken(const std::string& token) {
  static const std::vector<std::string> punctuation = {
      ".", ",", "!", "?", ";", ":", "。", "，", "！", "？", "；", "：", "、"};
  return std::find(punctuation.begin(), punctuation.end(), token) != punctuation.end();
}

std::string TrimTrailingSpaces(std::string text) {
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

std::string NormalizeTranscriptText(const std::string& text) {
  // Keep this in the backend so IBus, CLI, and future Fcitx5 all get the same
  // final text. If any CJK/Japanese/Korean character is present, normalize
  // punctuation to full-width; otherwise normalize to half-width. Collapse
  // punctuation runs even when ASR inserts spaces between them, e.g.
  // "？ . ." -> "？" and "。 。 . ." -> "。".
  const auto tokens = TokenizeUtf8(text);
  const bool cjk_text = ContainsCjk(tokens);
  std::string out;
  bool pending_space = false;

  for (size_t i = 0; i < tokens.size();) {
    if (IsAsciiSpace(tokens[i])) {
      pending_space = true;
      ++i;
      continue;
    }

    // Preserve dots used as number/version separators (e.g. 1.1.1)
    bool is_number_dot = false;
    if (tokens[i].text == ".") {
      // Check previous non-space output ends with a digit
      bool prev_is_digit = !out.empty() && IsDigit(std::string(1, out.back()));
      // Check next non-space token is a digit
      bool next_is_digit = false;
      for (size_t k = i + 1; k < tokens.size(); ++k) {
        if (IsAsciiSpace(tokens[k])) continue;
        next_is_digit = IsDigit(tokens[k].text);
        break;
      }
      is_number_dot = prev_is_digit && next_is_digit;
    }

    std::string token = is_number_dot ? "." : NormalizePunctuationToken(tokens[i].text, cjk_text);
    if (IsPunctuationToken(token)) {
      out = TrimTrailingSpaces(std::move(out));
      out += token;
      pending_space = false;

      bool saw_space_after_punct = false;
      size_t j = i + 1;
      while (j < tokens.size()) {
        if (IsAsciiSpace(tokens[j])) {
          saw_space_after_punct = true;
          ++j;
          continue;
        }
        std::string next = NormalizePunctuationToken(tokens[j].text, cjk_text);
        if (!IsPunctuationToken(next)) break;
        ++j;
      }
      pending_space = !cjk_text && saw_space_after_punct;
      i = j;
      continue;
    }

    if (pending_space && !out.empty()) {
      // CJK text commonly has no spaces around CJK punctuation, but keep word
      // separation for embedded Latin text and non-CJK text.
      if (!cjk_text || (tokens[i].codepoint < 0x80 &&
                        static_cast<unsigned char>(out.back()) < 0x80)) {
        out += ' ';
      }
    }
    out += token;
    pending_space = false;
    ++i;
  }
  return TrimTrailingSpaces(std::move(out));
}

std::string JoinSegments(const Session& session) {
  std::string out;
  for (const auto& [_, text] : session.segment_text) {
    if (!out.empty() && !text.empty()) out += " ";
    out += text;
  }
  return NormalizeTranscriptText(out);
}

std::vector<int> CompletedSegments(const Session& session) {
  std::vector<int> out;
  for (const auto& [idx, _] : session.segment_text) out.push_back(idx);
  return out;
}

xtils::Result<xtils::Json> InvalidParams(const std::string& message) {
  return xtils::Err(xtils::jsonrpc::kInvalidParams, message);
}

xtils::Json ErrorNotificationParams(const std::string& session_id,
                                    const std::string& code,
                                    const std::string& message) {
  xtils::Json params = xtils::Json::object();
  params["session_id"] = session_id;
  params["code"] = code;
  params["message"] = message;
  return params;
}

std::string ShellQuote(const std::string& value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

class ModelManager {
 public:
  ModelManager(vibetype::SenseVoiceEngine& engine, xtils::IpcServer& server,
               std::string model_path, std::string model_url)
      : engine_(engine), server_(server), model_path_(std::move(model_path)),
        model_url_(std::move(model_url)) {}

  ~ModelManager() {
    if (worker_.joinable()) worker_.join();
  }

  void StartAsync() { worker_ = std::thread(&ModelManager::EnsureAndLoad, this); }

  bool Ready() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_ == "ready";
  }

  xtils::Json StatusJson() const {
    std::lock_guard<std::mutex> lock(mu_);
    xtils::Json status = xtils::Json::object();
    status["state"] = state_;
    status["model_path"] = model_path_;
    status["model_url"] = model_url_;
    status["message"] = message_;
    return status;
  }

  std::string StateMessage() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_ + (message_.empty() ? "" : (": " + message_));
  }

 private:
  void SetState(const std::string& state, const std::string& message) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      state_ = state;
      message_ = message;
    }
    LogI("model status: %s %s", state.c_str(), message.c_str());
    server_.Notify("vibetype.modelStatusChanged", StatusJson());
  }

  bool DownloadModel() {
    std::filesystem::path path(model_path_);
    std::filesystem::create_directories(path.parent_path());
    const std::string tmp = model_path_ + ".part";
    const std::string cmd = "curl -L --fail --retry 3 --connect-timeout 20 -o " +
                            ShellQuote(tmp) + " " + ShellQuote(model_url_);
    LogI("downloading model: %s", model_url_.c_str());
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
      SetState("error", "model download failed; install curl or download model manually");
      return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, model_path_, ec);
    if (ec) {
      SetState("error", "failed to move downloaded model: " + ec.message());
      return false;
    }
    return true;
  }

  void EnsureAndLoad() {
    if (!std::filesystem::exists(model_path_)) {
      SetState("downloading", "model missing; downloading to " + model_path_);
      if (!DownloadModel()) return;
    }

    SetState("loading", "loading SenseVoice model");
    std::string error;
    if (!engine_.Load(model_path_, error)) {
      SetState("error", error);
      return;
    }
    SetState("ready", "model ready");
  }

  vibetype::SenseVoiceEngine& engine_;
  xtils::IpcServer& server_;
  std::string model_path_;
  std::string model_url_;
  mutable std::mutex mu_;
  std::string state_ = "starting";
  std::string message_;
  std::thread worker_;
};

class BackendService {
 public:
  BackendService(xtils::IpcServer& server, vibetype::SenseVoiceEngine& engine,
                 ModelManager* model_manager, bool fake_asr,
                 std::string fake_text, int threads)
      : server_(server), engine_(engine), model_manager_(model_manager),
        fake_asr_(fake_asr), fake_text_(std::move(fake_text)), threads_(threads) {}

  void RegisterMethods() {
    server_.Register("vibetype.hello", [this](const xtils::Json& params) { return Hello(params); });
    server_.Register("vibetype.startSession", [this](const xtils::Json& params) { return StartSession(params); });
    server_.Register("vibetype.transcribeSegment", [this](const xtils::Json& params) { return TranscribeSegment(params); });
    server_.Register("vibetype.finishSession", [this](const xtils::Json& params) { return FinishSession(params); });
    server_.Register("vibetype.cancelSession", [this](const xtils::Json& params) { return CancelSession(params); });
    server_.Register("vibetype.modelStatus", [this](const xtils::Json& params) { return ModelStatus(params); });
  }

 private:
  xtils::Result<xtils::Json> Hello(const xtils::Json&) {
    xtils::Json result = xtils::Json::object();
    result["protocol_version"] = 1;
    result["backend"] = "vibetype-backend";
    result["features"] = JsonArray(fake_asr_ ? std::vector<std::string>{"segment_asr", "partial_result", "final_result", "fake_asr"}
                                             : std::vector<std::string>{"segment_asr", "partial_result", "final_result", "sensevoice_asr", "model_status"});
    result["model"] = fake_asr_ ? FakeModelStatus() : model_manager_->StatusJson();
    return result;
  }

  xtils::Result<xtils::Json> ModelStatus(const xtils::Json&) {
    return fake_asr_ ? FakeModelStatus() : model_manager_->StatusJson();
  }

  xtils::Json FakeModelStatus() const {
    xtils::Json status = xtils::Json::object();
    status["state"] = "ready";
    status["model_path"] = "fake-asr";
    status["model_url"] = "";
    status["message"] = "fake ASR enabled";
    return status;
  }

  xtils::Result<xtils::Json> StartSession(const xtils::Json& params) {
    auto session_id = params.get_string("session_id");
    if (!session_id || session_id->empty()) return InvalidParams("missing session_id");

    const xtils::Json* audio = params.find("audio_format");
    if (!audio || audio->get_integer("sample_rate") != 16000 ||
        audio->get_integer("channels") != 1 ||
        audio->get_string("sample_format") != "pcm_s16le" ||
        audio->get_string("container") != "wav") {
      return xtils::Err(xtils::jsonrpc::kInvalidParams, "unsupported audio format");
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      sessions_[*session_id] = Session{};
    }
    LogI("start session %s", session_id->c_str());
    xtils::Json result = xtils::Json::object();
    result["accepted"] = true;
    return result;
  }

  xtils::Result<xtils::Json> TranscribeSegment(const xtils::Json& params) {
    auto session_id = params.get_string("session_id");
    auto segment_index = params.get_integer("segment_index");
    auto wav_path = params.get_string("wav_path");
    if (!session_id || !segment_index || !wav_path) {
      return InvalidParams("missing segment params");
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (sessions_.find(*session_id) == sessions_.end()) {
        return xtils::Err(-32004, "session not found");
      }
    }

    std::string code;
    std::string message;
    if (!ValidateAudioSegment(*wav_path, code, message)) {
      server_.Notify("vibetype.error", ErrorNotificationParams(*session_id, code, message));
      return xtils::Err(-32002, message);
    }

    std::string transcript;
    if (fake_asr_) {
      transcript = FakeTranscribeSegment(static_cast<int>(*segment_index), fake_text_);
    } else {
      if (!model_manager_->Ready()) {
        const std::string status = model_manager_->StateMessage();
        server_.Notify("vibetype.error", ErrorNotificationParams(*session_id, "model_not_ready", status));
        return xtils::Err(-32010, "model not ready: " + status);
      }
      std::string asr_error;
      {
        std::lock_guard<std::mutex> lock(engine_mu_);
        transcript = engine_.TranscribeFile(*wav_path, threads_, asr_error);
      }
      if (!asr_error.empty()) {
        server_.Notify("vibetype.error", ErrorNotificationParams(*session_id, "asr_failed", asr_error));
        return xtils::Err(-32003, asr_error);
      }
    }

    bool should_try_final = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = sessions_.find(*session_id);
      if (it == sessions_.end()) return xtils::Err(-32004, "session not found");
      it->second.segment_text[static_cast<int>(*segment_index)] = transcript;
      SendPartialLocked(*session_id, it->second);
      should_try_final = true;
    }
    if (should_try_final) TrySendFinal(*session_id);

    xtils::Json result = xtils::Json::object();
    result["accepted"] = true;
    result["session_id"] = *session_id;
    result["segment_index"] = *segment_index;
    return result;
  }

  xtils::Result<xtils::Json> FinishSession(const xtils::Json& params) {
    auto session_id = params.get_string("session_id");
    auto segment_count = params.get_integer("segment_count");
    if (!session_id || !segment_count) return InvalidParams("missing finish params");

    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = sessions_.find(*session_id);
      if (it == sessions_.end()) return xtils::Err(-32004, "session not found");
      it->second.finished = true;
      it->second.expected_count = static_cast<int>(*segment_count);
    }
    TrySendFinal(*session_id);

    xtils::Json result = xtils::Json::object();
    result["accepted"] = true;
    result["session_id"] = *session_id;
    return result;
  }

  xtils::Result<xtils::Json> CancelSession(const xtils::Json& params) {
    auto session_id = params.get_string("session_id");
    if (!session_id) return InvalidParams("missing session_id");
    {
      std::lock_guard<std::mutex> lock(mu_);
      sessions_.erase(*session_id);
    }
    xtils::Json result = xtils::Json::object();
    result["cancelled"] = true;
    return result;
  }

  void SendPartialLocked(const std::string& session_id, const Session& session) {
    xtils::Json params = xtils::Json::object();
    params["session_id"] = session_id;
    params["completed_segments"] = JsonArray(CompletedSegments(session));
    params["text"] = JoinSegments(session);
    params["is_final"] = false;
    server_.Notify("vibetype.partialResult", params);
  }

  void TrySendFinal(const std::string& session_id) {
    xtils::Json params = xtils::Json::object();
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = sessions_.find(session_id);
      if (it == sessions_.end()) return;
      Session& session = it->second;
      if (session.final_sent || !session.finished || session.expected_count < 0) return;
      if (static_cast<int>(session.segment_text.size()) < session.expected_count) return;
      for (int i = 0; i < session.expected_count; ++i) {
        if (session.segment_text.find(i) == session.segment_text.end()) return;
      }
      session.final_sent = true;
      params["session_id"] = session_id;
      params["segment_count"] = session.expected_count;
      params["text"] = JoinSegments(session);
      params["is_final"] = true;
    }
    server_.Notify("vibetype.finalResult", params);
  }

  xtils::IpcServer& server_;
  vibetype::SenseVoiceEngine& engine_;
  ModelManager* model_manager_ = nullptr;
  bool fake_asr_ = false;
  std::string fake_text_;
  int threads_ = 8;
  std::mutex mu_;
  std::mutex engine_mu_;
  std::map<std::string, Session> sessions_;
};

}  // namespace

int main(int argc, char** argv) {
  InstallGgmlLogger();

  xtils::Config config;
  config.Define("socket", "JSON-RPC Unix socket path", DefaultSocketPath()).Short("socket", "s")
      .Define("model", "SenseVoice GGUF model path", DefaultModelPath())
      .Define("model-url", "URL used to download the model when missing", DefaultModelUrl())
      .Define("threads", "ASR compute thread count", 8).Short("threads", "t")
      .Define("fake-asr", "Use fake ASR for protocol debugging", false)
      .Define("fake-text", "Transcript returned by fake ASR", "");

  if (!config.ParseArgs(argc, const_cast<const char**>(argv), true)) {
    return 1;
  }

  std::string socket_path = config.GetOr<std::string>("socket");
  std::string model_path = config.GetOr<std::string>("model");
  std::string model_url = config.GetOr<std::string>("model-url");
  int threads = config.GetOr<int>("threads");
  bool fake_asr = config.GetOr<bool>("fake-asr");
  std::string fake_text = config.GetOr<std::string>("fake-text");
  if (threads <= 0) threads = 1;

  if (!EnsureDir(DirName(socket_path))) {
    LogE("failed to create socket directory for %s", socket_path.c_str());
    return 1;
  }

  vibetype::SenseVoiceEngine engine;
  xtils::IpcServer server(socket_path);
  std::unique_ptr<ModelManager> model_manager;
  if (fake_asr) {
    LogW("starting with fake ASR engine");
  } else {
    model_manager = std::make_unique<ModelManager>(engine, server, model_path, model_url);
  }

  BackendService service(server, engine, model_manager.get(), fake_asr, fake_text, threads);
  service.RegisterMethods();

  xtils::system::SignalHandler::Initialize([&server] { server.Stop(); });

  if (!server.Start()) {
    LogE("failed to listen on %s", socket_path.c_str());
    xtils::system::SignalHandler::Cleanup();
    return 1;
  }
  LogI("listening on %s", socket_path.c_str());
  if (model_manager) model_manager->StartAsync();

  while (!xtils::system::SignalHandler::IsShutdownRequested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  server.Stop();
  xtils::system::SignalHandler::Cleanup();
  return 0;
}
