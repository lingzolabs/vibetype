#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "xtils/utils/json.h"
#include "xtils/logging/logger.h"

namespace vibetype {

// Backend runtime configuration loaded from backend.json and text-processing.json.
// Fields like socket/model/threads are read-once at startup (startup_* prefix);
// text-processing fields are hot-reloadable at runtime.
struct BackendConfig {
  // ── Startup fields (read from backend.json, overridable by explicit CLI args) ──
  std::string startup_socket;
  std::string startup_model;
  std::string startup_model_url;
  int startup_threads = 0;          // 0 = not set in file
  bool startup_fake_asr = false;
  std::string startup_fake_text;
  std::string startup_qwen_model;
  std::string startup_qwen_model_url;
  bool startup_qwen_enabled = false;
  int startup_qwen_threads = 0;     // 0 = not set in file

  // ── Hot-reloadable text-processing fields ──
  bool enable_builtin_corrections = true;
  std::map<std::string, std::string> custom_corrections;
  bool enable_qwen_polish = false;
  int qwen_max_tokens = 256;
  int qwen_n_ctx = 512;
  int qwen_timeout_ms = 10000;
  std::string qwen_system_prompt;
  std::string qwen_prompt_template;

  std::string loaded_at;
  time_t backend_mtime = 0;
  time_t text_proc_mtime = 0;
  // Last reload error (empty = ok).
  std::string last_error;
  // Monotonic revision counter, incremented on each successful reload.
  int revision = 0;
};

// Manages config files for backend.json and text-processing.json.
// Thread-safe: the active config snapshot is replaced atomically under a mutex.
class ConfigManager {
 public:
  ConfigManager(std::string backend_cfg_path, std::string text_proc_cfg_path,
                std::string data_dir)
      : backend_cfg_path_(std::move(backend_cfg_path)),
        text_proc_cfg_path_(std::move(text_proc_cfg_path)),
        data_dir_(std::move(data_dir)) {}

  // Load at startup (ignore missing files — use defaults).
  void LoadInitial();

  // Check mtime and reload if either file has changed since last load.
  // Returns true if config was reloaded (or had errors that updated last_error).
  // Will NOT spam logs if mtime check fails repeatedly — only logs on change.
  bool CheckAndReload();

  // Force reload regardless of mtime. Transactional: on parse error keeps old
  // config but updates last_error and returns false.
  bool ForceReload();

  // Get a snapshot of the current active config (cheap copy).
  BackendConfig GetConfig() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_;
  }

  // Status for vibetype.configStatus RPC.
  xtils::Json StatusJson() const;

  // Signal that SIGHUP was received; main loop should call CheckAndReload().
  void SetReloadFlag() { reload_flag_.store(true, std::memory_order_release); }

  // Check and clear the reload flag.
  bool TakeReloadFlag() {
    return reload_flag_.exchange(false, std::memory_order_acq_rel);
  }

 private:
  // Load built-in computer_terms.json corrections from data_dir_.
  std::map<std::string, std::string> LoadBuiltinTerms() const;

  // Actually load and merge both config files into a new BackendConfig.
  // Returns false (with error logged) if a parse error occurs.
  bool DoLoad(BackendConfig& out) const;

  std::string backend_cfg_path_;
  std::string text_proc_cfg_path_;
  std::string data_dir_;

  mutable std::mutex mu_;
  BackendConfig config_;
  std::atomic<bool> reload_flag_{false};
  // Track last known mtime to avoid redundant reload attempts.
  time_t last_checked_backend_mtime_ = -1;
  time_t last_checked_text_proc_mtime_ = -1;
};

}  // namespace vibetype
