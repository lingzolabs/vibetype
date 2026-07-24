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
#include "text_processor.h"

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
  uint64_t generation = 0;
  bool finished = false;
  int expected_count = -1;
  std::map<int, std::string> segment_text;
  bool final_sent = false;
};

std::string FakeTranscribeSegment(int segment_index, const std::string& fake_text) {
  if (!fake_text.empty()) return fake_text;
  return "fake transcript segment " + std::to_string(segment_index);
}

std::string JoinSegments(const Session& session,
                         const vibetype::TextProcessor& tp,
                         bool is_final) {
  std::string out;
  for (const auto& [_, text] : session.segment_text) {
    if (!out.empty() && !text.empty()) out += " ";
    out += text;
  }
  if (is_final) return tp.ProcessFinal(out);
  return tp.ProcessPartial(out);
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
                 std::string fake_text, int threads,
                 vibetype::TextProcessor text_processor)
      : server_(server), engine_(engine), model_manager_(model_manager),
        fake_asr_(fake_asr), fake_text_(std::move(fake_text)), threads_(threads),
        text_processor_(std::move(text_processor)) {}

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
      Session session;
      session.generation = ++next_session_generation_;
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
    if (!session_id || !segment_index || !wav_path) {
      return InvalidParams("missing segment params");
    }

    uint64_t session_generation = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto it = sessions_.find(*session_id);
      if (it == sessions_.end()) {
        return xtils::Err(-32004, "session not found");
      }
      session_generation = it->second.generation;
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

    // Build partial snapshot under lock, then process and notify outside.
    Session partial_snapshot;
    std::vector<int> partial_segments;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = sessions_.find(*session_id);
      if (it == sessions_.end() ||
          it->second.generation != session_generation) {
        return xtils::Err(-32004, "session was cancelled or replaced");
      }
      it->second.segment_text[static_cast<int>(*segment_index)] = transcript;
      partial_snapshot = it->second;
      partial_segments = CompletedSegments(it->second);
    }
    // Process partial text outside the lock, then re-check the session before
    // notifying. Holding the lock for Notify linearizes it with cancellation.
    const std::string partial_text =
        JoinSegments(partial_snapshot, text_processor_, false);
    {
      std::lock_guard<std::mutex> lock(mu_);
      const auto it = sessions_.find(*session_id);
      if (it == sessions_.end() ||
          it->second.generation != session_generation ||
          it->second.final_sent) {
        return xtils::Err(-32004, "session was cancelled, replaced, or finalized");
      }
      xtils::Json params = xtils::Json::object();
      params["session_id"] = *session_id;
      params["completed_segments"] = JsonArray(partial_segments);
      params["text"] = partial_text;
      params["is_final"] = false;
      server_.Notify("vibetype.partialResult", params);
    }
    TrySendFinal(*session_id);

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

  void TrySendFinal(const std::string& session_id) {
    // Step 1: Under lock, check readiness and collect a raw snapshot.
    Session raw_snapshot;
    int expected_count = -1;
    uint64_t session_generation = 0;
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
      // Mark final_sent NOW (under lock) to prevent duplicate sends.
      session.final_sent = true;
      raw_snapshot = session;
      expected_count = session.expected_count;
      session_generation = session.generation;
    }

    // Step 2: Process text OUTSIDE the lock (may be slow for long transcripts).
    const std::string final_text = JoinSegments(raw_snapshot, text_processor_, true);

    // Step 3: Second-check: session must still exist (not cancelled) and
    // final_sent must still be true (the flag we just set).
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = sessions_.find(session_id);
      if (it == sessions_.end() ||
          it->second.generation != session_generation ||
          !it->second.final_sent) {
        return;
      }
      xtils::Json params = xtils::Json::object();
      params["session_id"] = session_id;
      params["segment_count"] = expected_count;
      params["text"] = final_text;
      params["is_final"] = true;
      // Keep the lock while notifying so cancelSession cannot complete before
      // an already-approved final notification is emitted.
      server_.Notify("vibetype.finalResult", params);
    }
  }

  xtils::IpcServer& server_;
  vibetype::SenseVoiceEngine& engine_;
  ModelManager* model_manager_ = nullptr;
  bool fake_asr_ = false;
  std::string fake_text_;
  int threads_ = 8;
  vibetype::TextProcessor text_processor_;
  std::mutex mu_;
  std::mutex engine_mu_;
  uint64_t next_session_generation_ = 0;
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

  // FindDataDir: locate the text-processing data directory.
  // Priority:
  //   1. VIBETYPE_DATA_DIR environment variable
  //   2. Executable-relative ../share/vibetype (works from install tree)
  //   3. Compile-time VIBETYPE_INSTALL_DATA_DIR (after `make install`)
  //   4. Compile-time VIBETYPE_SOURCE_DATA_DIR (build tree / dev)
  auto FindDataDir = [&argv]() -> std::string {
    // 1. Env override
    const char* env_dd = std::getenv("VIBETYPE_DATA_DIR");
    if (env_dd && *env_dd) return env_dd;

    // 2. Executable-relative ../share/vibetype
    {
      std::error_code ec;
      auto exe = std::filesystem::canonical(
          std::filesystem::path(argv[0]), ec);
      if (!ec) {
        auto rel = exe.parent_path().parent_path() / "share" / "vibetype";
        if (std::filesystem::exists(rel / "text-processing.json", ec))
          return rel.string();
      }
    }

#ifdef VIBETYPE_INSTALL_DATA_DIR
    if (std::filesystem::exists(
            std::filesystem::path(VIBETYPE_INSTALL_DATA_DIR) / "text-processing.json"))
      return VIBETYPE_INSTALL_DATA_DIR;
#endif

#ifdef VIBETYPE_SOURCE_DATA_DIR
    if (std::filesystem::exists(
            std::filesystem::path(VIBETYPE_SOURCE_DATA_DIR) / "text-processing.json"))
      return VIBETYPE_SOURCE_DATA_DIR;
#endif

    return "";  // no data dir found; Load() will use hardcoded defaults
  };

  const std::string tp_data_dir = FindDataDir();
  auto tp_cfg = vibetype::TextProcessorConfig::Load(tp_data_dir);
  vibetype::TextProcessor text_processor(std::move(tp_cfg));
  LogI("text processor loaded with %zu alias entries (data dir: %s)",
       text_processor.Config().alias.entries.size(), tp_data_dir.c_str());

  BackendService service(server, engine, model_manager.get(), fake_asr, fake_text, threads,
                         std::move(text_processor));
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
