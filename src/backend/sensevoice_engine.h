#pragma once

#include <map>
#include <string>
#include <vector>

struct ggml_context;
struct ggml_tensor;

namespace vibetype {

class SenseVoiceEngine {
 public:
  SenseVoiceEngine() = default;
  ~SenseVoiceEngine();

  SenseVoiceEngine(const SenseVoiceEngine&) = delete;
  SenseVoiceEngine& operator=(const SenseVoiceEngine&) = delete;

  bool Load(const std::string& model_path, std::string& error);
  std::string TranscribeFile(const std::string& wav_path, int threads,
                             std::string& error);
  bool loaded() const { return ctx_w_ != nullptr; }

 private:
  struct Config {
    int d_model = 512;
    int n_head = 4;
    int num_blocks = 50;
    int tp_blocks = 20;
    int kernel = 11;
    int vocab = 25055;
    int blank = 0;
  };

  ggml_tensor* Tensor(const std::string& name, std::string& error);
  std::string TranscribeFeatures(const std::vector<float>& features, int frames,
                                 int threads, std::string& error);

  Config config_;
  ggml_context* ctx_w_ = nullptr;
  std::map<std::string, ggml_tensor*> tensors_;
  std::vector<int> query_tokens_;
  std::vector<std::string> vocab_;
};

}  // namespace vibetype
