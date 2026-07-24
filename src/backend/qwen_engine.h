#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Forward declarations from llama.h (C API)
struct llama_model;
struct llama_context;
struct llama_sampler;

namespace vibetype {

// Qwen3-0.6B text polishing engine using llama.cpp runtime.
// Default: disabled. Only activated when qwen_enabled = true in config.
// Model missing: triggers background download then load.
// Load/inference error: returns empty string so caller can fall back.
class QwenEngine {
 public:
  enum class State { kIdle, kDownloading, kLoading, kReady, kError };

  QwenEngine() = default;
  ~QwenEngine();

  QwenEngine(const QwenEngine&) = delete;
  QwenEngine& operator=(const QwenEngine&) = delete;

  // Start async download+load. Safe to call multiple times (no-op if already started).
  // n_ctx: context window size for inference (0 = use default 512).
  void StartAsync(const std::string& model_path, const std::string& model_url,
                  int threads, int n_ctx = 0);

  // Returns true only when model is fully loaded and ready for inference.
  bool Ready() const;

  State GetState() const;
  std::string GetStateMessage() const;

  // Run inference: polish/correct ASR text.
  // Returns polished text, or empty string on error/not-ready (caller fallback).
  // n_ctx: context size (0 = use stored value); timeout_ms: 0 = no timeout.
  std::string Polish(const std::string& text,
                     const std::string& system_prompt,
                     const std::string& legacy_prompt_template,
                     int max_tokens, int n_ctx = 0, int timeout_ms = 0);

 private:
  void WorkerThread(std::string model_path, std::string model_url, int threads,
                    int n_ctx);
  bool DownloadModel(const std::string& model_path, const std::string& model_url);
  bool LoadModel(const std::string& model_path, int threads, int n_ctx);
  void SetState(State s, const std::string& msg);

  // Strip <think>...</think> blocks from Qwen3 output.
  static std::string StripThinkBlocks(const std::string& text);

  mutable std::mutex mu_;
  State state_ = State::kIdle;
  std::string state_msg_;

  // llama resources (guarded by infer_mu_ during inference)
  std::mutex infer_mu_;
  struct llama_model* model_ = nullptr;
  struct llama_context* ctx_ = nullptr;
  struct llama_sampler* sampler_ = nullptr;
  int threads_ = 4;
  int n_ctx_ = 512;

  std::thread worker_;
  std::atomic<bool> started_{false};
};

}  // namespace vibetype
