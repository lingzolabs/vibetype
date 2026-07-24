#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config_manager.h"
#include "qwen_engine.h"
#include "sensevoice_engine.h"
#include "text_processor.h"

#include "ggml.h"

#include "xtils/config/config.h"
#include "xtils/logging/logger.h"
#include "xtils/net/ipc_channel.h"
#include "xtils/system/signal_handler.h"
#include "xtils/utils/json.h"
#include "xtils/utils/result.h"

namespace {

// ── Path helpers ────────────────────────────────────────────────────────────

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

std::string DefaultQwenModelPath() {
  return ConfigDir() + "/vibetype/models/qwen3-0.6b-q4_k_m.gguf";
}

std::string DefaultQwenModelUrl() {
  return "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/1208e45d782fe18602c5eaf10e5758d5b0f24c03/Qwen3-0.6B-Q4_K_M.gguf";
}

std::string DefaultBackendConfigPath() {
  return ConfigDir() + "/vibetype/backend.json";
}

std::string DefaultTextProcConfigPath() {
  return ConfigDir() + "/vibetype/text-processing.json";
}

// Determine install data dir. Priority:
//   1. VIBETYPE_INSTALL_DATA_DIR (compile-time install prefix)
//   2. Relative to executable: .../bin → .../share/vibetype (install)
//   3. Relative to executable: .../build/bin → .../data (source build)
//   4. VIBETYPE_SOURCE_DATA_DIR (compile-time source root)
//   5. /usr/share/vibetype (final fallback)
std::string FindDataDir() {
#ifdef VIBETYPE_INSTALL_DATA_DIR
  {
    const std::string install_data = VIBETYPE_INSTALL_DATA_DIR;
    if (std::filesystem::exists(install_data + "/computer_terms.json"))
      return install_data;
  }
#endif
  // Try relative to current executable.
  char buf[4096] = {};
  ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    std::string exe(buf, static_cast<size_t>(len));
    const size_t bin_pos = exe.rfind("/bin/");
    if (bin_pos != std::string::npos) {
      const std::string prefix = exe.substr(0, bin_pos);
      // Install layout: prefix/share/vibetype
      const std::string install_dir = prefix + "/share/vibetype";
      if (std::filesystem::exists(install_dir + "/computer_terms.json"))
        return install_dir;
      // Source build layout: prefix/../data  (e.g. build/../data)
      const size_t parent_slash = prefix.rfind('/');
      if (parent_slash != std::string::npos) {
        const std::string source_data =
            prefix.substr(0, parent_slash) + "/data";
        if (std::filesystem::exists(source_data + "/computer_terms.json"))
          return source_data;
      }
    }
  }
#ifdef VIBETYPE_SOURCE_DATA_DIR
  {
    const std::string src_data = VIBETYPE_SOURCE_DATA_DIR;
    if (std::filesystem::exists(src_data + "/computer_terms.json"))
      return src_data;
  }
#endif
  return "/usr/share/vibetype";
}

// ── Misc helpers ─────────────────────────────────────────────────────────────

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
    case GGML_LOG_LEVEL_DEBUG: LogD("ggml: %s", msg.c_str()); break;
    case GGML_LOG_LEVEL_WARN:  LogW("ggml: %s", msg.c_str()); break;
    case GGML_LOG_LEVEL_ERROR: LogE("ggml: %s", msg.c_str()); break;
    default:                   LogI("ggml: %s", msg.c_str()); break;
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
  if (!in) { error = "file_not_found"; return std::nullopt; }

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
        error = "invalid_fmt_chunk"; return std::nullopt;
      }
      info.audio_format    = ReadLe16(data.data());
      info.channels        = ReadLe16(data.data() + 2);
      info.sample_rate     = ReadLe32(data.data() + 4);
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
  if (!has_fmt) { error = "missing_fmt_chunk"; return std::nullopt; }
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

xtils::Json JsonArray(const std::vector<std::string>& v) {
  xtils::Json arr = xtils::Json::array();
  for (const auto& s : v) arr.push_back(s);
  return arr;
}

xtils::Json JsonArray(const std::vector<int>& v) {
  xtils::Json arr = xtils::Json::array();
  for (int x : v) arr.push_back(x);
  return arr;
}

// ── Normalization ────────────────────────────────────────────────────────────

struct Utf8Token { std::string text; uint32_t codepoint = 0; };

std::vector<Utf8Token> TokenizeUtf8(const std::string& text) {
  std::vector<Utf8Token> tokens;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t len = 1; uint32_t cp = c;
    if      ((c & 0xE0) == 0xC0 && i+1 < text.size()) { len=2; cp=((c&0x1F)<<6)|(static_cast<unsigned char>(text[i+1])&0x3F); }
    else if ((c & 0xF0) == 0xE0 && i+2 < text.size()) { len=3; cp=((c&0x0F)<<12)|((static_cast<unsigned char>(text[i+1])&0x3F)<<6)|(static_cast<unsigned char>(text[i+2])&0x3F); }
    else if ((c & 0xF8) == 0xF0 && i+3 < text.size()) { len=4; cp=((c&0x07)<<18)|((static_cast<unsigned char>(text[i+1])&0x3F)<<12)|((static_cast<unsigned char>(text[i+2])&0x3F)<<6)|(static_cast<unsigned char>(text[i+3])&0x3F); }
    tokens.push_back({text.substr(i, len), cp});
    i += len;
  }
  return tokens;
}

bool NeedsFullwidthPunct(uint32_t cp) {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x3040 && cp <= 0x30FF);
}

bool ContainsFullwidthPunct(const std::vector<Utf8Token>& tokens) {
  for (const auto& t : tokens) if (NeedsFullwidthPunct(t.codepoint)) return true;
  return false;
}

bool IsAsciiSpace(const Utf8Token& t) {
  return t.text==" " || t.text=="\t" || t.text=="\n" || t.text=="\r";
}

bool IsDigit(const std::string& s) { return s.size()==1 && s[0]>='0' && s[0]<='9'; }

std::string NormalizePunctuationToken(const std::string& t, bool cjk) {
  static const std::map<std::string,std::string> to_full={{".", "。"},{",", "，"},{"!", "！"},{"?", "？"},{";", "；"},{":", "："}};
  static const std::map<std::string,std::string> to_half={{"。", "."}, {"，", ","}, {"！", "!"}, {"？", "?"}, {"；", ";"}, {"：", ":"}, {"、", ","}};
  if (cjk) { auto it=to_full.find(t); return it==to_full.end() ? t : it->second; }
  auto it=to_half.find(t); return it==to_half.end() ? t : it->second;
}

bool IsPunctuationToken(const std::string& t) {
  static const std::vector<std::string> p={".", ",", "!", "?", ";", ":", "。", "，", "！", "？", "；", "：", "、"};
  return std::find(p.begin(), p.end(), t) != p.end();
}

std::string TrimTrailingSpaces(std::string text) {
  while (!text.empty() && (text.back()==' '||text.back()=='\t'||text.back()=='\n'||text.back()=='\r')) text.pop_back();
  return text;
}

std::string NormalizeTranscriptText(const std::string& text) {
  const auto tokens = TokenizeUtf8(text);
  const bool cjk_text = ContainsFullwidthPunct(tokens);
  std::string out; bool pending_space = false;
  for (size_t i = 0; i < tokens.size();) {
    if (IsAsciiSpace(tokens[i])) { pending_space=true; ++i; continue; }
    bool is_number_dot = false;
    if (tokens[i].text==".") {
      bool prev_is_digit = !out.empty() && IsDigit(std::string(1, out.back()));
      bool next_is_digit = false;
      for (size_t k=i+1; k<tokens.size(); ++k) { if (IsAsciiSpace(tokens[k])) continue; next_is_digit=IsDigit(tokens[k].text); break; }
      is_number_dot = prev_is_digit && next_is_digit;
    }
    std::string token = is_number_dot ? "." : NormalizePunctuationToken(tokens[i].text, cjk_text);
    if (IsPunctuationToken(token)) {
      out = TrimTrailingSpaces(std::move(out)); out += token; pending_space=false;
      bool saw_space_after_punct=false; size_t j=i+1;
      while (j<tokens.size()) {
        if (IsAsciiSpace(tokens[j])) { saw_space_after_punct=true; ++j; continue; }
        std::string nxt=NormalizePunctuationToken(tokens[j].text, cjk_text);
        if (!IsPunctuationToken(nxt)) break; ++j;
      }
      pending_space = !cjk_text && saw_space_after_punct; i=j; continue;
    }
    if (pending_space && !out.empty()) {
      if (!cjk_text || (tokens[i].codepoint<0x80 && static_cast<unsigned char>(out.back())<0x80)) out+=' ';
    }
    out+=token; pending_space=false; ++i;
  }
  return TrimTrailingSpaces(std::move(out));
}

// ── Session ──────────────────────────────────────────────────────────────────

struct Session {
  bool finished = false;
  int expected_count = -1;
  std::map<int, std::string> segment_text;  // normalized+corrected text per segment
  vibetype::BackendConfig text_config;
  bool final_sent = false;
};

std::string FakeTranscribeSegment(int idx, const std::string& fake_text) {
  return fake_text.empty() ? "fake transcript segment " + std::to_string(idx) : fake_text;
}

std::string JoinRaw(const Session& session) {
  std::string out;
  for (const auto& [_, text] : session.segment_text) {
    if (!out.empty() && !text.empty()) out += " ";
    out += text;
  }
  return out;
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
  xtils::Json p = xtils::Json::object();
  p["session_id"] = session_id;
  p["code"] = code;
  p["message"] = message;
  return p;
}

std::string ShellQuote(const std::string& value) {
  std::string out = "'";
  for (char c : value) { if (c == '\'') out += "'\\''"; else out += c; }
  out += "'"; return out;
}

// ── FinalResultWorkerPool ─────────────────────────────────────────────────────
// Replaces detached threads with a bounded worker pool. Tasks are queued and
// executed by a fixed-size thread pool; the pool is safely joined on destruction.

class FinalResultWorkerPool {
 public:
  explicit FinalResultWorkerPool(size_t num_workers = 2) {
    for (size_t i = 0; i < num_workers; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  ~FinalResultWorkerPool() {
    {
      std::unique_lock<std::mutex> lock(mu_);
      shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
      if (w.joinable()) w.join();
    }
  }

  // Submit a task. Returns immediately. Thread-safe.
  void Submit(std::function<void()> task) {
    {
      std::unique_lock<std::mutex> lock(mu_);
      queue_.push(std::move(task));
    }
    cv_.notify_one();
  }

 private:
  void WorkerLoop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
        if (shutdown_ && queue_.empty()) return;
        task = std::move(queue_.front());
        queue_.pop();
      }
      task();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> queue_;
  std::vector<std::thread> workers_;
  bool shutdown_ = false;
};

// ── ModelManager ─────────────────────────────────────────────────────────────

class ModelManager {
 public:
  ModelManager(vibetype::SenseVoiceEngine& engine, xtils::IpcServer& server,
               std::string model_path, std::string model_url)
      : engine_(engine), server_(server), model_path_(std::move(model_path)),
        model_url_(std::move(model_url)) {}

  ~ModelManager() { if (worker_.joinable()) worker_.join(); }

  void StartAsync() { worker_ = std::thread(&ModelManager::EnsureAndLoad, this); }

  bool Ready() const { std::lock_guard<std::mutex> l(mu_); return state_=="ready"; }

  xtils::Json StatusJson() const {
    std::lock_guard<std::mutex> l(mu_);
    xtils::Json s = xtils::Json::object();
    s["state"] = state_; s["model_path"] = model_path_;
    s["model_url"] = model_url_; s["message"] = message_;
    return s;
  }

  std::string StateMessage() const {
    std::lock_guard<std::mutex> l(mu_);
    return state_ + (message_.empty() ? "" : (": " + message_));
  }

 private:
  void SetState(const std::string& state, const std::string& message) {
    { std::lock_guard<std::mutex> l(mu_); state_=state; message_=message; }
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
    if (rc != 0) { SetState("error", "model download failed; install curl or download model manually"); return false; }
    std::error_code ec;
    std::filesystem::rename(tmp, model_path_, ec);
    if (ec) { SetState("error", "failed to move downloaded model: " + ec.message()); return false; }
    return true;
  }

  void EnsureAndLoad() {
    if (!std::filesystem::exists(model_path_)) {
      SetState("downloading", "model missing; downloading to " + model_path_);
      if (!DownloadModel()) return;
    }
    SetState("loading", "loading SenseVoice model");
    std::string error;
    if (!engine_.Load(model_path_, error)) { SetState("error", error); return; }
    SetState("ready", "model ready");
  }

  vibetype::SenseVoiceEngine& engine_;
  xtils::IpcServer& server_;
  std::string model_path_, model_url_;
  mutable std::mutex mu_;
  std::string state_ = "starting", message_;
  std::thread worker_;
};

// ── BackendService ────────────────────────────────────────────────────────────

class BackendService {
 public:
  BackendService(xtils::IpcServer& server,
                 vibetype::SenseVoiceEngine& engine,
                 ModelManager* model_manager,
                 vibetype::QwenEngine* qwen_engine,
                 vibetype::ConfigManager& config_manager,
                 bool fake_asr,
                 std::string fake_text,
                 int threads,
                 bool qwen_allowed)
      : server_(server), engine_(engine), model_manager_(model_manager),
        qwen_engine_(qwen_engine), config_manager_(config_manager),
        fake_asr_(fake_asr), fake_text_(std::move(fake_text)), threads_(threads),
        qwen_allowed_(qwen_allowed) {}

  // Must be called before server starts.
  void SetQwenParams(std::string model_path, std::string model_url, int qwen_threads) {
    qwen_model_path_ = std::move(model_path);
    qwen_model_url_ = std::move(model_url);
    qwen_threads_ = qwen_threads;
  }

  void RegisterMethods() {
    server_.Register("vibetype.hello",
        [this](const xtils::Json& p) { return Hello(p); });
    server_.Register("vibetype.startSession",
        [this](const xtils::Json& p) { return StartSession(p); });
    server_.Register("vibetype.transcribeSegment",
        [this](const xtils::Json& p) { return TranscribeSegment(p); });
    server_.Register("vibetype.finishSession",
        [this](const xtils::Json& p) { return FinishSession(p); });
    server_.Register("vibetype.cancelSession",
        [this](const xtils::Json& p) { return CancelSession(p); });
    server_.Register("vibetype.modelStatus",
        [this](const xtils::Json& p) { return ModelStatus(p); });
    server_.Register("vibetype.reloadConfig",
        [this](const xtils::Json& p) { return ReloadConfig(p); });
    server_.Register("vibetype.configStatus",
        [this](const xtils::Json& p) { return ConfigStatus(p); });
    server_.Register("vibetype.polishStatus",
        [this](const xtils::Json& p) { return PolishStatus(p); });
  }

  // Called by main loop when SIGHUP flag fires (immediate) or mtime check fires.
  // Returns true if config was reloaded.
  bool MaybeReloadConfig(bool force = false) {
    bool reloaded = false;
    if (force) {
      reloaded = config_manager_.ForceReload();
    } else {
      if (config_manager_.TakeReloadFlag()) {
        reloaded = config_manager_.ForceReload();
      } else {
        reloaded = config_manager_.CheckAndReload();
      }
    }
    if (reloaded) {
      LogI("config: hot-reloaded");
      ApplyRuntimeConfigChanges();
      xtils::Json params = xtils::Json::object();
      params["reason"] = "config_reloaded";
      params["revision"] =
          static_cast<int64_t>(config_manager_.GetConfig().revision);
      server_.Notify("vibetype.statusChanged", params);
    }
    return reloaded;
  }

 private:
  // After a successful config reload, apply changes that can be acted on at runtime.
  // Currently: if enable_qwen_polish became true and qwen_engine_ is not yet started,
  // start it now.
  void ApplyRuntimeConfigChanges() {
    if (!qwen_engine_ || !qwen_allowed_) return;
    const auto cfg = config_manager_.GetConfig();
    if (cfg.enable_qwen_polish &&
        cfg.qwen_system_prompt.empty() && cfg.qwen_prompt_template.empty()) {
      LogE("config: Qwen polishing enabled but no prompt is configured; skipping Qwen");
      return;
    }
    if (cfg.enable_qwen_polish) {
      const auto state = qwen_engine_->GetState();
      if (state == vibetype::QwenEngine::State::kIdle) {
        LogI("config: qwen_polish enabled via reload, starting QwenEngine");
        qwen_engine_->StartAsync(qwen_model_path_, qwen_model_url_, qwen_threads_,
                                 cfg.qwen_n_ctx);
      }
    }
  }

  // Build features list.
  std::vector<std::string> Features() const {
    std::vector<std::string> f = {"segment_asr", "partial_result", "final_result",
                                  "config_reload", "text_corrections"};
    if (fake_asr_) {
      f.push_back("fake_asr");
    } else {
      f.push_back("sensevoice_asr");
      f.push_back("model_status");
    }
    if (qwen_allowed_) {
      f.push_back("qwen_polish");
      f.push_back("polish_status");
    }
    return f;
  }

  xtils::Result<xtils::Json> Hello(const xtils::Json&) {
    xtils::Json result = xtils::Json::object();
    result["protocol_version"] = 1;
    result["backend"] = "vibetype-backend";
    result["features"] = JsonArray(Features());
    result["model"] = fake_asr_ ? FakeModelStatus() : model_manager_->StatusJson();
    result["qwen"] = QwenStatusJson();
    // Emit status notification so clients receive an unprompted hello.
    xtils::Json status = xtils::Json::object();
    status["backend"] = "vibetype-backend";
    status["features"] = JsonArray(Features());
    server_.Notify("vibetype.statusChanged", status);
    return result;
  }

  xtils::Result<xtils::Json> ModelStatus(const xtils::Json&) {
    return fake_asr_ ? FakeModelStatus() : model_manager_->StatusJson();
  }

  xtils::Json FakeModelStatus() const {
    xtils::Json s = xtils::Json::object();
    s["state"] = "ready"; s["model_path"] = "fake-asr";
    s["model_url"] = ""; s["message"] = "fake ASR enabled";
    return s;
  }

  xtils::Result<xtils::Json> ReloadConfig(const xtils::Json&) {
    const bool ok = config_manager_.ForceReload();
    xtils::Json result = xtils::Json::object();
    result["ok"] = ok;
    auto status = config_manager_.StatusJson();
    status["qwen"] = QwenStatusJson();
    result["status"] = status;
    if (!ok) {
      const auto cfg = config_manager_.GetConfig();
      result["error"] = cfg.last_error.empty() ? "reload failed" : cfg.last_error;
    }
    if (ok) {
      ApplyRuntimeConfigChanges();
      xtils::Json params = xtils::Json::object();
      params["reason"] = "config_reloaded";
      params["revision"] =
          static_cast<int64_t>(config_manager_.GetConfig().revision);
      server_.Notify("vibetype.statusChanged", params);
    }
    return result;
  }

  xtils::Json QwenStatusJson() const {
    xtils::Json status = xtils::Json::object();
    const auto cfg = config_manager_.GetConfig();
    const bool prompt_configured =
        !cfg.qwen_system_prompt.empty() || !cfg.qwen_prompt_template.empty();
    status["prompt_configured"] = prompt_configured;
    status["enabled"] =
        cfg.enable_qwen_polish && qwen_allowed_ && prompt_configured;
    if (cfg.enable_qwen_polish && qwen_allowed_ && !prompt_configured) {
      status["state"] = "error";
      status["message"] =
          "Qwen polishing enabled but qwen_system_prompt is missing or empty";
      status["model_path"] = qwen_model_path_;
      return status;
    }
    if (!qwen_engine_) {
      status["state"] = "disabled";
      status["message"] = "Qwen engine unavailable";
      return status;
    }
    const auto state = qwen_engine_->GetState();
    status["state"] = state == vibetype::QwenEngine::State::kReady ? "ready" :
                      state == vibetype::QwenEngine::State::kLoading ? "loading" :
                      state == vibetype::QwenEngine::State::kDownloading ? "downloading" :
                      state == vibetype::QwenEngine::State::kError ? "error" : "idle";
    status["message"] = qwen_engine_->GetStateMessage();
    status["model_path"] = qwen_model_path_;
    return status;
  }

  xtils::Result<xtils::Json> ConfigStatus(const xtils::Json&) {
    xtils::Json status = config_manager_.StatusJson();
    status["qwen"] = QwenStatusJson();
    xtils::Json startup_only = xtils::Json::array();
    for (const char* field : {"socket", "model", "model_url", "threads",
                              "fake_asr", "qwen_model", "qwen_model_url",
                              "qwen_threads"}) {
      startup_only.push_back(field);
    }
    status["startup_only_fields"] = startup_only;
    return status;
  }

  xtils::Result<xtils::Json> PolishStatus(const xtils::Json&) {
    return QwenStatusJson();
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

    // Check mtime on startSession and apply any runtime changes.
    if (config_manager_.CheckAndReload()) {
      ApplyRuntimeConfigChanges();
      xtils::Json params2 = xtils::Json::object();
      params2["reason"] = "config_reloaded";
      params2["revision"] =
          static_cast<int64_t>(config_manager_.GetConfig().revision);
      server_.Notify("vibetype.statusChanged", params2);
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      Session session;
      session.text_config = config_manager_.GetConfig();
      sessions_[*session_id] = std::move(session);
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
    if (!session_id || !segment_index || !wav_path)
      return InvalidParams("missing segment params");

    {
      std::lock_guard<std::mutex> lock(mu_);
      if (sessions_.find(*session_id) == sessions_.end())
        return xtils::Err(-32004, "session not found");
    }

    std::string code, message;
    if (!ValidateAudioSegment(*wav_path, code, message)) {
      server_.Notify("vibetype.error", ErrorNotificationParams(*session_id, code, message));
      return xtils::Err(-32002, message);
    }

    std::string raw_transcript;
    if (fake_asr_) {
      raw_transcript = FakeTranscribeSegment(static_cast<int>(*segment_index), fake_text_);
    } else {
      if (!model_manager_->Ready()) {
        const std::string st = model_manager_->StateMessage();
        server_.Notify("vibetype.error",
            ErrorNotificationParams(*session_id, "model_not_ready", st));
        return xtils::Err(-32010, "model not ready: " + st);
      }
      std::string asr_error;
      {
        std::lock_guard<std::mutex> lock(engine_mu_);
        raw_transcript = engine_.TranscribeFile(*wav_path, threads_, asr_error);
      }
      if (!asr_error.empty()) {
        server_.Notify("vibetype.error",
            ErrorNotificationParams(*session_id, "asr_failed", asr_error));
        return xtils::Err(-32003, asr_error);
      }
    }

    // Partial pipeline: normalize → apply corrections (no Qwen).
    const std::string normalized = NormalizeTranscriptText(raw_transcript);

    bool should_try_final = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = sessions_.find(*session_id);
      if (it == sessions_.end()) return xtils::Err(-32004, "session not found");
      // Store normalized ASR text; corrections are applied when rendering
      // partial/final results so the final pipeline runs exactly once.
      it->second.segment_text[static_cast<int>(*segment_index)] = normalized;
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
    { std::lock_guard<std::mutex> lock(mu_); sessions_.erase(*session_id); }
    xtils::Json result = xtils::Json::object();
    result["cancelled"] = true;
    return result;
  }

  void SendPartialLocked(const std::string& session_id, const Session& session) {
    xtils::Json p = xtils::Json::object();
    p["session_id"] = session_id;
    p["completed_segments"] = JsonArray(CompletedSegments(session));
    const std::string joined = JoinRaw(session);
    vibetype::TextProcessor tp;
    p["text"] = tp.ProcessPartial(joined, session.text_config);
    p["is_final"] = false;
    server_.Notify("vibetype.partialResult", p);
  }

  // Submit final result computation to the worker pool.
  // Final pipeline: join normalized segments, normalize boundaries, apply
  // corrections, optional Qwen, corrections again, then final normalization.
  void TrySendFinal(const std::string& session_id) {
    bool can_send = false;
    std::string joined;
    int expected_count = 0;
    vibetype::BackendConfig session_config;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = sessions_.find(session_id);
      if (it == sessions_.end()) return;
      Session& s = it->second;
      if (s.final_sent || !s.finished || s.expected_count < 0) return;
      if (static_cast<int>(s.segment_text.size()) < s.expected_count) return;
      for (int i = 0; i < s.expected_count; ++i) {
        if (s.segment_text.find(i) == s.segment_text.end()) return;
      }
      s.final_sent = true;
      joined = JoinRaw(s);
      expected_count = s.expected_count;
      session_config = s.text_config;
      can_send = true;
    }
    if (!can_send) return;

    // Submit to thread pool; no capture of 'this' raw pointer in a detached thread.
    // The pool is owned by BackendService and outlives all tasks.
    const std::string sid = session_id;
    const int ec = expected_count;
    // We capture 'this' safely because BackendService outlives the pool (pool is
    // a member and joins all workers in its destructor before BackendService
    // members are destroyed).
    final_pool_.Submit([this, sid, joined, ec, session_config]() {
      vibetype::TextProcessor tp;

      // Final pipeline: join text (already normalized+corrected per segment)
      // → re-normalize joined text → corrections → Qwen → corrections → normalize.
      const std::string re_normalized = NormalizeTranscriptText(joined);
      const std::string polished_or_fallback =
          tp.ProcessFinal(re_normalized, session_config,
                          qwen_allowed_ ? qwen_engine_ : nullptr);
      const std::string final_text = NormalizeTranscriptText(polished_or_fallback);

      xtils::Json p = xtils::Json::object();
      p["session_id"] = sid;
      p["segment_count"] = ec;
      p["text"] = final_text;
      p["is_final"] = true;
      server_.Notify("vibetype.finalResult", p);

      { std::lock_guard<std::mutex> lock(mu_); sessions_.erase(sid); }
    });
  }

  xtils::IpcServer& server_;
  vibetype::SenseVoiceEngine& engine_;
  ModelManager* model_manager_ = nullptr;
  vibetype::QwenEngine* qwen_engine_ = nullptr;
  vibetype::ConfigManager& config_manager_;
  bool fake_asr_ = false;
  std::string fake_text_;
  int threads_ = 8;
  bool qwen_allowed_ = true;
  // Qwen engine params (for on-demand start after reload).
  std::string qwen_model_path_;
  std::string qwen_model_url_;
  int qwen_threads_ = 4;
  std::mutex mu_;
  std::mutex engine_mu_;
  std::map<std::string, Session> sessions_;
  // Worker pool for final result processing — must be declared after all other
  // members it accesses so it is destroyed (joined) first.
  FinalResultWorkerPool final_pool_{2};
};

}  // namespace

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  InstallGgmlLogger();

  const std::string data_dir = FindDataDir();
  LogI("data dir: %s", data_dir.c_str());

  // ── Parse CLI args ─────────────────────────────────────────────────────────
  // We use sentinel values to detect which CLI args were explicitly provided.
  // Empty string = "not set on CLI"; integer 0 = "not set on CLI".
  xtils::Config config;
  config.Define("socket",      "JSON-RPC Unix socket path", std::string(""))
        .Short("socket", "s")
        .Define("model",       "SenseVoice GGUF model path", std::string(""))
        .Define("model-url",   "URL to download SenseVoice model when missing",
                std::string(""))
        .Define("threads",     "ASR compute thread count (0 = use config/default)", 0)
        .Short("threads", "t")
        .Define("fake-asr",    "Use fake ASR for protocol debugging", false)
        .Define("fake-text",   "Transcript returned by fake ASR", std::string(""))
        .Define("qwen-model",  "Qwen3-0.6B GGUF model path", std::string(""))
        .Define("qwen-model-url", "URL to download Qwen model", std::string(""))
        .Define("qwen-enabled","Enable Qwen text polishing for testing", false)
        .Define("qwen-disabled","Disable Qwen text polishing for testing", false)
        .Define("qwen-threads","Qwen inference thread count (0 = use config/default)", 0)
        .Define("backend-config",  "Path to backend.json", std::string(""))
        .Define("text-proc-config","Path to text-processing.json", std::string(""));

  if (!config.ParseArgs(argc, const_cast<const char**>(argv), true)) return 1;

  // ── Determine config file paths ───────────────────────────────────────────
  std::string backend_cfg = config.GetOr<std::string>("backend-config");
  if (backend_cfg.empty()) backend_cfg = DefaultBackendConfigPath();
  std::string text_proc_cfg = config.GetOr<std::string>("text-proc-config");
  if (text_proc_cfg.empty()) text_proc_cfg = DefaultTextProcConfigPath();

  // ── Load config files first ───────────────────────────────────────────────
  vibetype::ConfigManager cfg_mgr(backend_cfg, text_proc_cfg, data_dir);
  cfg_mgr.LoadInitial();
  const vibetype::BackendConfig file_cfg = cfg_mgr.GetConfig();

  // ── Merge: file provides defaults; explicit CLI args override ─────────────
  // socket
  std::string cli_socket = config.GetOr<std::string>("socket");
  const std::string socket_path =
      !cli_socket.empty() ? cli_socket :
      !file_cfg.startup_socket.empty() ? file_cfg.startup_socket :
      DefaultSocketPath();

  // model
  std::string cli_model = config.GetOr<std::string>("model");
  const std::string model_path =
      !cli_model.empty() ? cli_model :
      !file_cfg.startup_model.empty() ? file_cfg.startup_model :
      DefaultModelPath();

  // model-url
  std::string cli_model_url = config.GetOr<std::string>("model-url");
  const std::string model_url =
      !cli_model_url.empty() ? cli_model_url :
      !file_cfg.startup_model_url.empty() ? file_cfg.startup_model_url :
      DefaultModelUrl();

  // threads (0 sentinel = not set on CLI)
  const int cli_threads = config.GetOr<int>("threads");
  const int threads = std::max(1,
      cli_threads > 0 ? cli_threads :
      file_cfg.startup_threads > 0 ? file_cfg.startup_threads : 8);

  // fake-asr: CLI flag takes precedence; file default is false.
  const bool cli_fake_asr = config.GetOr<bool>("fake-asr");
  const bool fake_asr = cli_fake_asr || file_cfg.startup_fake_asr;

  // fake-text
  std::string cli_fake_text = config.GetOr<std::string>("fake-text");
  const std::string fake_text =
      !cli_fake_text.empty() ? cli_fake_text : file_cfg.startup_fake_text;

  // qwen-model
  std::string cli_qwen_model = config.GetOr<std::string>("qwen-model");
  const std::string qwen_model =
      !cli_qwen_model.empty() ? cli_qwen_model :
      !file_cfg.startup_qwen_model.empty() ? file_cfg.startup_qwen_model :
      DefaultQwenModelPath();

  // qwen-model-url
  std::string cli_qwen_url = config.GetOr<std::string>("qwen-model-url");
  const std::string qwen_model_url =
      !cli_qwen_url.empty() ? cli_qwen_url :
      !file_cfg.startup_qwen_model_url.empty() ? file_cfg.startup_qwen_model_url :
      DefaultQwenModelUrl();

  // Qwen CLI flags are test-only explicit overrides of file settings.
  const bool cli_qwen_enabled = config.GetOr<bool>("qwen-enabled");
  const bool cli_qwen_disabled = config.GetOr<bool>("qwen-disabled");
  if (cli_qwen_enabled && cli_qwen_disabled) {
    LogE("--qwen-enabled and --qwen-disabled are mutually exclusive");
    return 1;
  }
  const bool file_qwen_enabled = file_cfg.startup_qwen_enabled ||
                                 file_cfg.enable_qwen_polish;
  const bool qwen_requested = cli_qwen_enabled ? true :
                              cli_qwen_disabled ? false : file_qwen_enabled;
  const bool qwen_prompt_configured =
      !file_cfg.qwen_system_prompt.empty() ||
      !file_cfg.qwen_prompt_template.empty();
  const bool qwen_enabled = qwen_requested && qwen_prompt_configured;
  if (qwen_requested && !qwen_prompt_configured) {
    LogE("Qwen requested but qwen_system_prompt is missing or empty; "
         "Qwen polishing is disabled");
  }

  // qwen-threads (0 sentinel = not set on CLI)
  const int cli_qwen_threads = config.GetOr<int>("qwen-threads");
  const int qwen_threads = std::max(1,
      cli_qwen_threads > 0 ? cli_qwen_threads :
      file_cfg.startup_qwen_threads > 0 ? file_cfg.startup_qwen_threads : 4);

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

  // Qwen engine: allocate if needed at startup, or if config says enable_qwen_polish.
  // The engine is also started on-demand if reload turns qwen_polish on.
  // Keep an idle engine object so a text-config reload can enable polishing.
  auto qwen_engine = std::make_unique<vibetype::QwenEngine>();

  BackendService service(server, engine, model_manager.get(), qwen_engine.get(),
                         cfg_mgr, fake_asr, fake_text, threads,
                         !cli_qwen_disabled);
  service.SetQwenParams(qwen_model, qwen_model_url, qwen_threads);
  service.RegisterMethods();

  // SIGHUP: set reload flag, handled in main loop within 100ms.
  static std::atomic<bool> sighup_flag{false};
  {
    struct sigaction sa = {};
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = [](int) { sighup_flag.store(true, std::memory_order_release); };
    ::sigaction(SIGHUP, &sa, nullptr);
  }

  xtils::system::SignalHandler::Initialize([&server] { server.Stop(); });

  if (!server.Start()) {
    LogE("failed to listen on %s", socket_path.c_str());
    xtils::system::SignalHandler::Cleanup();
    return 1;
  }
  LogI("listening on %s", socket_path.c_str());
  LogI("config: backend=%s text-proc=%s", backend_cfg.c_str(), text_proc_cfg.c_str());
  LogI("threads=%d qwen_enabled=%s qwen_threads=%d", threads,
       qwen_enabled ? "true" : "false", qwen_threads);

  if (model_manager) model_manager->StartAsync();
  if (qwen_enabled) {
    qwen_engine->StartAsync(qwen_model, qwen_model_url, qwen_threads,
                            file_cfg.qwen_n_ctx);
  }

  // Main loop: handle SIGHUP within 100ms; check mtime every ~5 seconds.
  int mtime_tick = 0;
  while (!xtils::system::SignalHandler::IsShutdownRequested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // SIGHUP: immediate reload (within 100ms).
    if (sighup_flag.exchange(false, std::memory_order_acq_rel)) {
      LogI("SIGHUP received — reloading config");
      service.MaybeReloadConfig(/*force=*/true);
      mtime_tick = 0;  // reset mtime counter after forced reload
      continue;
    }

    // Every ~5 seconds: check mtime.
    if (++mtime_tick >= 50) {
      mtime_tick = 0;
      service.MaybeReloadConfig(/*force=*/false);
    }
  }

  server.Stop();
  xtils::system::SignalHandler::Cleanup();
  return 0;
}
