#include "config_manager.h"

#include <sys/stat.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace vibetype {
namespace {

// Maximum number of custom correction rules allowed.
constexpr int kMaxCustomCorrections = 500;
// Maximum byte length for a correction key or value.
constexpr int kMaxCorrectionLen = 256;

std::string NowIso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  char buf[32] = {};
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
  return buf;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  if (!in) return "";
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

time_t GetMtime(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return 0;
  return st.st_mtime;
}

// Parse custom_corrections from a JSON value that is either:
//   - object:  { "from": "to", ... }
//   - array:   [ { "from": "...", "to": "..." }, ... ]
// Returns empty map on unrecognized format.
std::map<std::string, std::string> ParseCustomCorrections(const xtils::Json& j) {
  std::map<std::string, std::string> result;

  auto add_entry = [&](const std::string& from, const std::string& to) {
    if (from.empty()) return;  // reject empty key
    if (static_cast<int>(from.size()) > kMaxCorrectionLen ||
        static_cast<int>(to.size()) > kMaxCorrectionLen) return;
    if (static_cast<int>(result.size()) >= kMaxCustomCorrections) return;
    result[from] = to;
  };

  if (j.is_object()) {
    for (const auto& [k, v] : j.as_object()) {
      if (v.is_string()) add_entry(k, v.as_string());
    }
  } else if (j.is_array()) {
    for (const auto& item : j.as_array()) {
      if (!item.is_object()) continue;
      auto from = item.get_string("from");
      auto to   = item.get_string("to");
      if (from && to) add_entry(*from, *to);
    }
  }
  return result;
}

void ApplyTextProcessingFields(const xtils::Json& j, BackendConfig& cfg) {
  auto builtin = j.get_bool("enable_builtin_corrections");
  if (builtin) cfg.enable_builtin_corrections = *builtin;
  auto qwen_polish = j.get_bool("enable_qwen_polish");
  if (qwen_polish) cfg.enable_qwen_polish = *qwen_polish;
  auto max_tokens = j.get_integer("qwen_max_tokens");
  if (max_tokens) cfg.qwen_max_tokens = static_cast<int>(*max_tokens);
  auto n_ctx = j.get_integer("qwen_n_ctx");
  if (n_ctx) cfg.qwen_n_ctx = static_cast<int>(*n_ctx);
  auto timeout_ms = j.get_integer("qwen_timeout_ms");
  if (timeout_ms) cfg.qwen_timeout_ms = static_cast<int>(*timeout_ms);

  if (j.find("qwen_system_prompt")) {
    auto prompt = j.get_string("qwen_system_prompt");
    cfg.qwen_system_prompt = prompt ? *prompt : "";
    cfg.qwen_prompt_template.clear();
  }
  if (j.find("qwen_prompt_template")) {
    auto legacy = j.get_string("qwen_prompt_template");
    cfg.qwen_prompt_template = legacy ? *legacy : "";
  }

  const xtils::Json* custom = j.find("custom_corrections");
  if (custom) cfg.custom_corrections = ParseCustomCorrections(*custom);
}

}  // namespace

std::map<std::string, std::string> ConfigManager::LoadBuiltinTerms() const {
  std::map<std::string, std::string> result;
  const std::string terms_path = data_dir_ + "/computer_terms.json";
  const std::string text = ReadFile(terms_path);
  if (text.empty()) {
    LogW("config: could not load builtin terms from %s", terms_path.c_str());
    return result;
  }

  auto j = xtils::Json::parse(text);
  if (!j) {
    LogW("config: failed to parse %s", terms_path.c_str());
    return result;
  }
  const xtils::Json* corrections = j->find("corrections");
  if (!corrections || !corrections->is_object()) return result;
  for (const auto& [k, v] : corrections->as_object()) {
    if (v.is_string()) result[k] = v.as_string();
  }
  return result;
}

bool ConfigManager::DoLoad(BackendConfig& out) const {
  BackendConfig cfg;

  cfg.enable_builtin_corrections = true;

  auto apply_backend_fields = [&](const xtils::Json& j) {
    auto socket = j.get_string("socket");
    if (socket && !socket->empty()) cfg.startup_socket = *socket;
    auto model = j.get_string("model");
    if (model && !model->empty()) cfg.startup_model = *model;
    auto model_url = j.get_string("model_url");
    if (model_url && !model_url->empty()) cfg.startup_model_url = *model_url;
    auto threads = j.get_integer("threads");
    if (threads && *threads > 0) cfg.startup_threads = static_cast<int>(*threads);
    auto fake_asr = j.get_bool("fake_asr");
    if (fake_asr) cfg.startup_fake_asr = *fake_asr;
    auto fake_text = j.get_string("fake_text");
    if (fake_text) cfg.startup_fake_text = *fake_text;
    auto qwen_model = j.get_string("qwen_model");
    if (qwen_model && !qwen_model->empty()) cfg.startup_qwen_model = *qwen_model;
    auto qwen_model_url = j.get_string("qwen_model_url");
    if (qwen_model_url && !qwen_model_url->empty())
      cfg.startup_qwen_model_url = *qwen_model_url;
    auto qwen_enabled = j.get_bool("qwen_enabled");
    if (qwen_enabled) cfg.startup_qwen_enabled = *qwen_enabled;
    auto qwen_threads = j.get_integer("qwen_threads");
    if (qwen_threads && *qwen_threads > 0)
      cfg.startup_qwen_threads = static_cast<int>(*qwen_threads);
    ApplyTextProcessingFields(j, cfg);
  };

  auto load_json = [&](const std::string& path, bool user_file,
                       const auto& apply) -> bool {
    const std::string text = ReadFile(path);
    if (text.empty()) {
      if (user_file && GetMtime(path) != 0) {
        out.last_error = "empty or unreadable config: " + path;
        LogE("config: %s", out.last_error.c_str());
        return false;
      }
      return true;
    }
    auto j = xtils::Json::parse(text);
    if (!j) {
      out.last_error = "failed to parse " + path;
      LogE("config: %s", out.last_error.c_str());
      return false;
    }
    apply(*j);
    return true;
  };

  // Layer defaults first, then user overrides. The installed data files are
  // defaults, while ~/.config/vibetype/*.json contains only user choices.
  const std::string default_backend = data_dir_ + "/backend.json";
  const std::string default_text_proc = data_dir_ + "/text-processing.json";
  if (!load_json(default_backend, false, apply_backend_fields) ||
      !load_json(default_text_proc, false,
                 [&](const xtils::Json& j) { ApplyTextProcessingFields(j, cfg); }) ||
      !load_json(backend_cfg_path_, true, apply_backend_fields) ||
      !load_json(text_proc_cfg_path_, true,
                 [&](const xtils::Json& j) { ApplyTextProcessingFields(j, cfg); })) {
    return false;
  }

  // ── Merge built-in terms (lower priority than user custom rules) ─────────
  if (cfg.enable_builtin_corrections) {
    const auto terms = LoadBuiltinTerms();
    for (const auto& [k, v] : terms) {
      if (cfg.custom_corrections.find(k) == cfg.custom_corrections.end()) {
        cfg.custom_corrections[k] = v;
      }
    }
  }

  cfg.loaded_at = NowIso8601();
  cfg.backend_mtime = GetMtime(backend_cfg_path_);
  cfg.text_proc_mtime = GetMtime(text_proc_cfg_path_);
  cfg.last_error.clear();
  out = std::move(cfg);
  return true;
}

void ConfigManager::LoadInitial() {
  BackendConfig cfg;
  if (!DoLoad(cfg)) {
    LogW("config: initial load failed, using defaults");
    cfg = BackendConfig{};
    cfg.loaded_at = NowIso8601();
    cfg.last_error = "initial load failed";
    if (cfg.enable_builtin_corrections) {
      cfg.custom_corrections = LoadBuiltinTerms();
    }
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    config_ = std::move(cfg);
    // Track mtimes so we don't re-trigger on the first mtime check.
    last_checked_backend_mtime_ = config_.backend_mtime;
    last_checked_text_proc_mtime_ = config_.text_proc_mtime;
  }
}

bool ConfigManager::CheckAndReload() {
  const time_t cur_b = GetMtime(backend_cfg_path_);
  const time_t cur_t = GetMtime(text_proc_cfg_path_);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (cur_b == last_checked_backend_mtime_ &&
        cur_t == last_checked_text_proc_mtime_) {
      return false;
    }
    // Update tracked mtimes before attempting reload so a failing file doesn't
    // spam repeated reload attempts on every tick.
    last_checked_backend_mtime_ = cur_b;
    last_checked_text_proc_mtime_ = cur_t;
  }
  return ForceReload();
}

bool ConfigManager::ForceReload() {
  BackendConfig cfg;
  if (!DoLoad(cfg)) {
    LogW("config: reload failed, keeping previous config");
    // Record error in snapshot so StatusJson() can report it.
    std::lock_guard<std::mutex> lock(mu_);
    config_.last_error = cfg.last_error.empty() ? "reload failed" : cfg.last_error;
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  cfg.revision = config_.revision + 1;
  last_checked_backend_mtime_ = cfg.backend_mtime;
  last_checked_text_proc_mtime_ = cfg.text_proc_mtime;
  config_ = std::move(cfg);
  LogI("config: reloaded at %s (revision %d)", config_.loaded_at.c_str(),
       config_.revision);
  return true;
}

xtils::Json ConfigManager::StatusJson() const {
  std::lock_guard<std::mutex> lock(mu_);
  xtils::Json j = xtils::Json::object();
  j["default_backend_config_path"] = data_dir_ + "/backend.json";
  j["default_text_proc_config_path"] = data_dir_ + "/text-processing.json";
  j["backend_config_path"] = backend_cfg_path_;
  j["text_proc_config_path"] = text_proc_cfg_path_;
  j["loaded_at"] = config_.loaded_at;
  j["revision"] = static_cast<int64_t>(config_.revision);
  j["enable_builtin_corrections"] = config_.enable_builtin_corrections;
  j["enable_qwen_polish"] = config_.enable_qwen_polish;
  j["custom_corrections_count"] =
      static_cast<int64_t>(config_.custom_corrections.size());
  if (!config_.last_error.empty()) j["error"] = config_.last_error;
  return j;
}

}  // namespace vibetype
