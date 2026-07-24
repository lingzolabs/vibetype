#include "qwen_engine.h"

#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "llama.h"
#include "xtils/logging/logger.h"

namespace vibetype {
namespace {

std::string ShellQuote(const std::string& v) {
  std::string out = "'";
  for (char c : v) {
    if (c == '\'') out += "'\\''";
    else out += c;
  }
  out += "'";
  return out;
}

static void LlamaLogCallback(ggml_log_level level, const char* text, void*) {
  if (!text) return;
  std::string msg(text);
  while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
  if (msg.empty()) return;
  switch (level) {
    case GGML_LOG_LEVEL_WARN:  LogW("qwen(llama): %s", msg.c_str()); break;
    case GGML_LOG_LEVEL_ERROR: LogE("qwen(llama): %s", msg.c_str()); break;
    case GGML_LOG_LEVEL_DEBUG: LogD("qwen(llama): %s", msg.c_str()); break;
    default:                   LogI("qwen(llama): %s", msg.c_str()); break;
  }
}

}  // namespace

QwenEngine::~QwenEngine() {
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(infer_mu_);
  if (sampler_) { llama_sampler_free(sampler_); sampler_ = nullptr; }
  if (ctx_)     { llama_free(ctx_); ctx_ = nullptr; }
  if (model_)   { llama_model_free(model_); model_ = nullptr; }
  if (started_.load(std::memory_order_acquire)) llama_backend_free();
}

void QwenEngine::SetState(State s, const std::string& msg) {
  std::lock_guard<std::mutex> lock(mu_);
  state_ = s;
  state_msg_ = msg;
  LogI("qwen state: %s%s",
       s == State::kIdle        ? "idle"        :
       s == State::kDownloading ? "downloading" :
       s == State::kLoading     ? "loading"     :
       s == State::kReady       ? "ready"       : "error",
       msg.empty() ? "" : (" – " + msg).c_str());
}

bool QwenEngine::Ready() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_ == State::kReady;
}

QwenEngine::State QwenEngine::GetState() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

std::string QwenEngine::GetStateMessage() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_msg_;
}

void QwenEngine::StartAsync(const std::string& model_path,
                            const std::string& model_url, int threads,
                            int n_ctx) {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) return;
  llama_backend_init();
  llama_log_set(LlamaLogCallback, nullptr);
  worker_ = std::thread(&QwenEngine::WorkerThread, this, model_path, model_url,
                        threads, n_ctx);
}

bool QwenEngine::DownloadModel(const std::string& model_path,
                               const std::string& model_url) {
  std::filesystem::create_directories(std::filesystem::path(model_path).parent_path());
  const std::string tmp = model_path + ".qwen.part";
  const std::string cmd = "curl -L --fail --retry 3 --connect-timeout 20 -o " +
                          ShellQuote(tmp) + " " + ShellQuote(model_url);
  LogI("qwen: downloading model from %s", model_url.c_str());
  if (std::system(cmd.c_str()) != 0) {
    SetState(State::kError, "qwen model download failed");
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, model_path, ec);
  if (ec) {
    SetState(State::kError, "qwen rename failed: " + ec.message());
    return false;
  }
  return true;
}

bool QwenEngine::LoadModel(const std::string& model_path, int threads, int n_ctx) {
  SetState(State::kLoading, "loading Qwen3-0.6B model");

  auto mparams = llama_model_default_params();
  mparams.n_gpu_layers = 0;  // CPU only

  struct llama_model* model = llama_model_load_from_file(model_path.c_str(), mparams);
  if (!model) {
    SetState(State::kError, "failed to load qwen model: " + model_path);
    return false;
  }

  const int effective_n_ctx = n_ctx > 0 ? n_ctx : 512;
  auto cparams = llama_context_default_params();
  cparams.n_ctx = static_cast<uint32_t>(effective_n_ctx);
  cparams.n_threads = threads > 0 ? threads : 4;
  cparams.n_threads_batch = cparams.n_threads;

  struct llama_context* ctx = llama_init_from_model(model, cparams);
  if (!ctx) {
    llama_model_free(model);
    SetState(State::kError, "failed to create qwen context");
    return false;
  }

  auto sparams = llama_sampler_chain_default_params();
  struct llama_sampler* sampler = llama_sampler_chain_init(sparams);
  llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.1f));
  llama_sampler_chain_add(sampler, llama_sampler_init_greedy());

  {
    std::lock_guard<std::mutex> lock(infer_mu_);
    model_ = model;
    ctx_ = ctx;
    sampler_ = sampler;
    threads_ = threads;
    n_ctx_ = effective_n_ctx;
  }
  SetState(State::kReady, "Qwen3-0.6B ready");
  return true;
}

void QwenEngine::WorkerThread(std::string model_path, std::string model_url,
                              int threads, int n_ctx) {
  if (!std::filesystem::exists(model_path)) {
    SetState(State::kDownloading, "model missing; downloading to " + model_path);
    if (!DownloadModel(model_path, model_url)) return;
  }
  LoadModel(model_path, threads, n_ctx);
}

// static
std::string QwenEngine::StripThinkBlocks(const std::string& text) {
  // Remove <think>...</think> blocks (possibly multiline, non-greedy).
  std::string out;
  size_t pos = 0;
  const std::string open_tag = "<think>";
  const std::string close_tag = "</think>";
  while (pos < text.size()) {
    const size_t open = text.find(open_tag, pos);
    if (open == std::string::npos) {
      out.append(text, pos, std::string::npos);
      break;
    }
    // Append text before the <think> block.
    out.append(text, pos, open - pos);
    const size_t close = text.find(close_tag, open + open_tag.size());
    if (close == std::string::npos) {
      // Unclosed <think> — drop the rest.
      break;
    }
    pos = close + close_tag.size();
  }
  // Trim leading/trailing whitespace left by stripped blocks.
  const size_t first = out.find_first_not_of(" \t\n\r");
  if (first == std::string::npos) return "";
  const size_t last  = out.find_last_not_of(" \t\n\r");
  return out.substr(first, last - first + 1);
}

std::string QwenEngine::Polish(
    const std::string& text, const std::string& system_prompt,
    const std::string& legacy_prompt_template, int max_tokens,
    int /*n_ctx*/, int timeout_ms) {
  if (!Ready()) return "";
  if (text.empty()) return text;
  if (system_prompt.empty() && legacy_prompt_template.empty()) {
    LogW("qwen: polishing skipped because no prompt is configured");
    return "";
  }

  std::lock_guard<std::mutex> lock(infer_mu_);
  if (!model_ || !ctx_ || !sampler_) return "";

  std::string prompt;
  if (!legacy_prompt_template.empty()) {
    // Keep old user configurations working while new configs stay marker-free.
    prompt = legacy_prompt_template;
    const std::string placeholder = "{text}";
    const size_t pos = prompt.find(placeholder);
    if (pos != std::string::npos) {
      prompt.replace(pos, placeholder.size(), text);
    } else {
      prompt += "\n" + text;
    }
  } else {
    const std::string user_text =
        "/no_think\n请校正下面的 ASR 转写。只返回校正后的文本：\n"
        "<transcript>\n" +
        text + "\n</transcript>";
    const llama_chat_message messages[] = {
        {"system", system_prompt.c_str()},
        {"user", user_text.c_str()},
    };
    const char* chat_template = llama_model_chat_template(model_, nullptr);
    std::vector<char> formatted(
        (system_prompt.size() + user_text.size()) * 2 + 256);
    int32_t length = llama_chat_apply_template(
        chat_template, messages, 2, true, formatted.data(),
        static_cast<int32_t>(formatted.size()));
    if (length > static_cast<int32_t>(formatted.size())) {
      formatted.resize(static_cast<size_t>(length) + 1);
      length = llama_chat_apply_template(
          chat_template, messages, 2, true, formatted.data(),
          static_cast<int32_t>(formatted.size()));
    }
    if (length < 0 || length > static_cast<int32_t>(formatted.size())) {
      LogW("qwen: failed to apply model chat template");
      return "";
    }
    prompt.assign(formatted.data(), static_cast<size_t>(length));
  }

  const struct llama_vocab* vocab = llama_model_get_vocab(model_);

  // Tokenize prompt.
  std::vector<llama_token> tokens(prompt.size() + 64);
  int n = llama_tokenize(vocab, prompt.c_str(),
                         static_cast<int>(prompt.size()),
                         tokens.data(),
                         static_cast<int>(tokens.size()),
                         /*add_special=*/true,
                         /*parse_special=*/true);
  if (n < 0) {
    tokens.resize(static_cast<size_t>(-n) + 16);
    n = llama_tokenize(vocab, prompt.c_str(),
                       static_cast<int>(prompt.size()),
                       tokens.data(),
                       static_cast<int>(tokens.size()),
                       true, true);
    if (n < 0) {
      LogW("qwen: tokenize failed");
      return "";
    }
  }
  tokens.resize(static_cast<size_t>(n));

  // Clear KV cache for fresh generation.
  llama_memory_clear(llama_get_memory(ctx_), /*data=*/false);

  // Decode prompt tokens.
  {
    const struct llama_batch batch =
        llama_batch_get_one(tokens.data(), static_cast<int>(tokens.size()));
    if (llama_decode(ctx_, batch) != 0) {
      LogW("qwen: prompt decode failed");
      return "";
    }
  }

  // Generate output tokens with optional timeout.
  std::string output;
  const int n_generate = max_tokens > 0 ? max_tokens : 256;
  const auto deadline =
      timeout_ms > 0
          ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)
          : std::chrono::steady_clock::time_point::max();

  bool timed_out = false;
  for (int i = 0; i < n_generate; ++i) {
    // llama.cpp decoding cannot be interrupted mid-token, but stop between tokens.
    if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
      LogW("qwen: inference timeout after %d tokens", i);
      timed_out = true;
      break;
    }

    const llama_token id = llama_sampler_sample(sampler_, ctx_, -1);
    if (llama_vocab_is_eog(vocab, id)) break;

    char piece[256] = {};
    const int plen = llama_token_to_piece(vocab, id, piece, sizeof(piece) - 1,
                                          0, /*special=*/false);
    if (plen > 0) {
      output.append(piece, plen);
    }

    llama_token next_id = id;
    const struct llama_batch batch = llama_batch_get_one(&next_id, 1);
    if (llama_decode(ctx_, batch) != 0) {
      LogW("qwen: generation decode failed at token %d", i);
      break;
    }
  }

  // A timed-out partial sentence is less safe than the deterministic fallback.
  if (timed_out) return "";

  // Strip any <think>...</think> blocks Qwen3 may emit even when instructed not to.
  const std::string stripped = StripThinkBlocks(output);

  // Empty output protection: return empty so caller falls back to rule-based.
  if (stripped.empty()) return "";

  return stripped;
}

}  // namespace vibetype
