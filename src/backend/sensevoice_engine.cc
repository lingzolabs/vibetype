#include "sensevoice_engine.h"

#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"
#include "gguf.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <utility>

#include "xtils/logging/logger.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace vibetype {
namespace {

constexpr float kLnEps = 1e-5f;
constexpr int kFs = 16000;
constexpr int kWinLen = 400;
constexpr int kShift = 160;
constexpr int kNfft = 512;
constexpr int kNmel = 80;
constexpr int kLfrM = 7;
constexpr int kLfrN = 6;
constexpr float kPreEmph = 0.97f;
constexpr float kLowF = 20.0f;
constexpr float kHighF = 8000.0f;

float Mel(float f) { return 1127.0f * logf(1.0f + f / 700.0f); }

uint16_t ReadLe16(const unsigned char* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const unsigned char* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

std::optional<std::vector<float>> LoadPcm16MonoWav(const std::string& path,
                                                   std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "failed to open wav: " + path;
    return std::nullopt;
  }

  unsigned char riff[12] = {};
  in.read(reinterpret_cast<char*>(riff), sizeof(riff));
  if (in.gcount() != static_cast<std::streamsize>(sizeof(riff)) ||
      std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
    error = "not a RIFF/WAVE file";
    return std::nullopt;
  }

  bool saw_fmt = false;
  bool valid_fmt = false;
  std::vector<unsigned char> pcm;
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
        error = "invalid fmt chunk";
        return std::nullopt;
      }
      saw_fmt = true;
      const uint16_t audio_format = ReadLe16(data.data());
      const uint16_t channels = ReadLe16(data.data() + 2);
      const uint32_t sample_rate = ReadLe32(data.data() + 4);
      const uint16_t bits_per_sample = ReadLe16(data.data() + 14);
      valid_fmt = audio_format == 1 && channels == 1 && sample_rate == 16000 && bits_per_sample == 16;
    } else if (id == "data") {
      pcm.resize(size);
      in.read(reinterpret_cast<char*>(pcm.data()), size);
      if (in.gcount() != static_cast<std::streamsize>(size)) {
        error = "truncated data chunk";
        return std::nullopt;
      }
    } else {
      in.seekg(size, std::ios::cur);
    }
    if (size % 2 == 1) in.seekg(1, std::ios::cur);
  }

  if (!saw_fmt || !valid_fmt) {
    error = "expected 16 kHz mono PCM16 WAV";
    return std::nullopt;
  }
  if (pcm.empty() || pcm.size() % 2 != 0) {
    error = "empty or invalid PCM data";
    return std::nullopt;
  }

  std::vector<float> out(pcm.size() / 2);
  for (size_t i = 0; i < out.size(); ++i) {
    int16_t sample = static_cast<int16_t>(ReadLe16(&pcm[i * 2]));
    out[i] = static_cast<float>(sample) / 32768.0f;
  }
  return out;
}

void Fft(std::vector<float>& re, std::vector<float>& im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int b = n >> 1;
    for (; j & b; b >>= 1) j ^= b;
    j ^= b;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    double a = -2.0 * M_PI / len;
    float wr = cosf(a), wi = sinf(a);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        float ur = re[i + k], ui = im[i + k];
        float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr;
        im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr;
        im[i + k + len / 2] = ui - vi;
        float nc = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = nc;
      }
    }
  }
}

std::vector<float> ComputeFbank(std::vector<float> wav, int& frames_out) {
  for (auto& v : wav) v *= 32768.0f;
  std::vector<float> win(kWinLen);
  for (int i = 0; i < kWinLen; i++) {
    win[i] = 0.54f - 0.46f * cosf(2.0f * M_PI * i / (kWinLen - 1));
  }

  constexpr int kNbin = kNfft / 2 + 1;
  float bw = static_cast<float>(kFs) / kNfft;
  float ml = Mel(kLowF), mh = Mel(kHighF), dm = (mh - ml) / (kNmel + 1);
  std::vector<std::vector<float>> fb(kNmel, std::vector<float>(kNbin, 0.0f));
  for (int m = 0; m < kNmel; m++) {
    float l = ml + m * dm;
    float c = ml + (m + 1) * dm;
    float r = ml + (m + 2) * dm;
    for (int k = 0; k < kNbin; k++) {
      float mf = Mel(bw * k);
      if (mf > l && mf < r) fb[m][k] = mf <= c ? (mf - l) / (c - l) : (r - mf) / (r - c);
    }
  }

  int n = static_cast<int>(wav.size());
  int t = n >= kWinLen ? (n - kWinLen) / kShift + 1 : 0;
  if (t <= 0) {
    frames_out = 0;
    return {};
  }

  std::vector<std::vector<float>> feat(t, std::vector<float>(kNmel));
  std::vector<float> re(kNfft), im(kNfft), fr(kWinLen);
  constexpr float floor = 1.1920929e-07f;
  for (int frame = 0; frame < t; frame++) {
    const float* s = wav.data() + frame * kShift;
    double mean = 0;
    for (int i = 0; i < kWinLen; i++) mean += s[i];
    mean /= kWinLen;
    for (int i = 0; i < kWinLen; i++) fr[i] = s[i] - static_cast<float>(mean);
    for (int i = kWinLen - 1; i > 0; i--) fr[i] -= kPreEmph * fr[i - 1];
    fr[0] -= kPreEmph * fr[0];
    for (int i = 0; i < kNfft; i++) {
      re[i] = i < kWinLen ? fr[i] * win[i] : 0.0f;
      im[i] = 0.0f;
    }
    Fft(re, im, kNfft);
    for (int m = 0; m < kNmel; m++) {
      float e = 0;
      for (int k = 0; k < kNbin; k++) {
        if (fb[m][k] > 0) e += fb[m][k] * (re[k] * re[k] + im[k] * im[k]);
      }
      feat[frame][m] = logf(e > floor ? e : floor);
    }
  }

  constexpr int pad = (kLfrM - 1) / 2;
  int tl = (t + kLfrN - 1) / kLfrN;
  std::vector<std::vector<float>> padded;
  padded.reserve(t + pad + kLfrM);
  for (int i = 0; i < pad; i++) padded.push_back(feat[0]);
  for (int i = 0; i < t; i++) padded.push_back(feat[i]);
  while (static_cast<int>(padded.size()) < (tl - 1) * kLfrN + kLfrM) padded.push_back(feat[t - 1]);

  constexpr int dim = kLfrM * kNmel;
  std::vector<float> out(static_cast<size_t>(tl) * dim);
  for (int i = 0; i < tl; i++) {
    for (int j = 0; j < kLfrM; j++) {
      memcpy(&out[(static_cast<size_t>(i) * dim) + j * kNmel], padded[i * kLfrN + j].data(), kNmel * sizeof(float));
    }
  }
  frames_out = tl;
  return out;
}

void AddPosEnc(std::vector<float>& x, int frames, int depth) {
  double inc = log(10000.0) / (depth / 2.0 - 1.0);
  for (int t = 0; t < frames; t++) {
    double pos = t + 1;
    for (int i = 0; i < depth / 2; i++) {
      double its = exp(i * -inc);
      double st = pos * its;
      x[static_cast<size_t>(t) * depth + i] += static_cast<float>(sin(st));
      x[static_cast<size_t>(t) * depth + depth / 2 + i] += static_cast<float>(cos(st));
    }
  }
}

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(' ');
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(' ');
  return s.substr(a, b - a + 1);
}

std::string Detok(const std::vector<int>& ids, const std::vector<std::string>& vocab) {
  std::string s;
  for (int id : ids) {
    if (id < 0 || id >= static_cast<int>(vocab.size())) continue;
    const std::string& p = vocab[id];
    if (p.size() >= 2 && p[0] == '<' && p[1] == '|') continue;
    s += p;
  }
  const std::string lb = "\xe2\x96\x81";
  size_t pos;
  while ((pos = s.find(lb)) != std::string::npos) s.replace(pos, 3, " ");
  return Trim(s);
}

}  // namespace

SenseVoiceEngine::~SenseVoiceEngine() {
  if (ctx_w_) ggml_free(ctx_w_);
}

bool SenseVoiceEngine::Load(const std::string& model_path, std::string& error) {
  if (ctx_w_) {
    ggml_free(ctx_w_);
    ctx_w_ = nullptr;
  }
  tensors_.clear();
  query_tokens_.clear();
  vocab_.clear();

  gguf_init_params gp = {false, &ctx_w_};
  gguf_context* gg = gguf_init_from_file(model_path.c_str(), gp);
  if (!gg) {
    error = "failed to load gguf model: " + model_path;
    return false;
  }

  auto read_u32 = [&](const char* key, int fallback) {
    int idx = gguf_find_key(gg, key);
    return idx < 0 ? fallback : static_cast<int>(gguf_get_val_u32(gg, idx));
  };
  config_.d_model = read_u32("sv.output_size", 512);
  config_.n_head = read_u32("sv.attention_heads", 4);
  config_.num_blocks = read_u32("sv.num_blocks", 50);
  config_.tp_blocks = read_u32("sv.tp_blocks", 20);
  config_.kernel = read_u32("sv.kernel_size", 11);
  config_.vocab = read_u32("sv.vocab_size", 25055);
  config_.blank = read_u32("sv.blank_id", 0);

  int qi = gguf_find_key(gg, "sv.query_tokens");
  int nq = qi < 0 ? 0 : static_cast<int>(gguf_get_arr_n(gg, qi));
  query_tokens_.resize(nq);
  for (int i = 0; i < nq; i++) {
    query_tokens_[i] = static_cast<const int32_t*>(gguf_get_arr_data(gg, qi))[i];
  }

  int vi = gguf_find_key(gg, "sv.vocab");
  if (vi >= 0) {
    int nv = static_cast<int>(gguf_get_arr_n(gg, vi));
    vocab_.resize(nv);
    for (int i = 0; i < nv; i++) {
      const char* s = gguf_get_arr_str(gg, vi, i);
      vocab_[i] = s ? s : "";
    }
  }

  for (int i = 0; i < gguf_get_n_tensors(gg); i++) {
    const char* name = gguf_get_tensor_name(gg, i);
    tensors_[name] = ggml_get_tensor(ctx_w_, name);
  }
  gguf_free(gg);

  if (query_tokens_.empty()) {
    error = "model missing sv.query_tokens";
    return false;
  }
  if (vocab_.empty()) {
    error = "model missing sv.vocab";
    return false;
  }
  std::string tensor_error;
  if (!Tensor("embed.weight", tensor_error)) {
    error = tensor_error;
    return false;
  }
  LogI("Loaded SenseVoice model %s", model_path.c_str());
  return true;
}

ggml_tensor* SenseVoiceEngine::Tensor(const std::string& name, std::string& error) {
  auto it = tensors_.find(name);
  if (it == tensors_.end() || !it->second) {
    error = "missing tensor: " + name;
    return nullptr;
  }
  return it->second;
}

std::string SenseVoiceEngine::TranscribeFile(const std::string& wav_path, int threads,
                                             std::string& error) {
  if (!loaded()) {
    error = "SenseVoice model is not loaded";
    return "";
  }
  auto wav = LoadPcm16MonoWav(wav_path, error);
  if (!wav) return "";
  int frames = 0;
  auto features = ComputeFbank(std::move(*wav), frames);
  if (frames <= 0 || features.empty()) {
    error = "audio too short";
    return "";
  }
  return TranscribeFeatures(features, frames, threads, error);
}

std::string SenseVoiceEngine::TranscribeFeatures(const std::vector<float>& features,
                                                 int frames, int threads,
                                                 std::string& error) {
  constexpr int input_dim = 560;
  const int d = config_.d_model;
  const int v = config_.vocab;
  const int n = static_cast<int>(query_tokens_.size()) + frames;

  ggml_tensor* embed_tensor = Tensor("embed.weight", error);
  if (!embed_tensor) return "";
  auto* emb = static_cast<float*>(embed_tensor->data);
  std::vector<float> input(static_cast<size_t>(n) * input_dim);
  for (size_t i = 0; i < query_tokens_.size(); i++) {
    memcpy(&input[i * input_dim], &emb[static_cast<size_t>(query_tokens_[i]) * input_dim], input_dim * sizeof(float));
  }
  memcpy(&input[static_cast<size_t>(query_tokens_.size()) * input_dim], features.data(), static_cast<size_t>(frames) * input_dim * sizeof(float));

  float scale = sqrtf(static_cast<float>(d));
  for (auto& value : input) value *= scale;
  AddPosEnc(input, n, input_dim);

  auto lin = [&](ggml_context* c, ggml_tensor* w, ggml_tensor* b, ggml_tensor* x) {
    auto y = ggml_mul_mat(c, w, x);
    return b ? ggml_add(c, y, b) : y;
  };
  auto lnorm = [&](ggml_context* c, ggml_tensor* x, ggml_tensor* g, ggml_tensor* b) {
    return ggml_add(c, ggml_mul(c, ggml_norm(c, x, kLnEps), g), b);
  };
  auto get = [&](const std::string& name) -> ggml_tensor* { return Tensor(name, error); };

  auto sanm_attn = [&](ggml_context* c, const std::string& p, ggml_tensor* x, int t) -> ggml_tensor* {
    const int h = config_.n_head;
    const int dk = d / h;
    const int kernel = config_.kernel;
    ggml_tensor* qkv = lin(c, get(p + "linear_q_k_v.weight"), get(p + "linear_q_k_v.bias"), x);
    size_t nb1 = qkv->nb[1];
    ggml_tensor* q = ggml_cont(c, ggml_view_2d(c, qkv, d, t, nb1, 0));
    ggml_tensor* k = ggml_cont(c, ggml_view_2d(c, qkv, d, t, nb1, static_cast<size_t>(d) * sizeof(float)));
    ggml_tensor* val = ggml_cont(c, ggml_view_2d(c, qkv, d, t, nb1, static_cast<size_t>(2 * d) * sizeof(float)));
    const int pad = (kernel - 1) / 2;
    ggml_tensor* fk = get(p + "fsmn_block.weight");
    ggml_tensor* vp = ggml_pad_ext(c, val, 0, 0, pad, pad, 0, 0, 0, 0);
    ggml_tensor* fsmn = val;
    for (int j = 0; j < kernel; j++) {
      auto sl = ggml_view_2d(c, vp, d, t, vp->nb[1], static_cast<size_t>(j) * vp->nb[1]);
      auto wj = ggml_view_1d(c, fk, d, static_cast<size_t>(j) * fk->nb[1]);
      fsmn = ggml_add(c, fsmn, ggml_mul(c, ggml_cont(c, sl), wj));
    }
    q = ggml_permute(c, ggml_reshape_3d(c, q, dk, h, t), 0, 2, 1, 3);
    k = ggml_permute(c, ggml_reshape_3d(c, k, dk, h, t), 0, 2, 1, 3);
    ggml_tensor* vh = ggml_cont(c, ggml_permute(c, ggml_reshape_3d(c, val, dk, h, t), 1, 2, 0, 3));
    ggml_tensor* kq = ggml_soft_max(c, ggml_scale(c, ggml_mul_mat(c, k, q), 1.0f / sqrtf(static_cast<float>(dk))));
    ggml_tensor* o = ggml_cont_2d(c, ggml_permute(c, ggml_mul_mat(c, vh, kq), 0, 2, 1, 3), d, t);
    return ggml_add(c, lin(c, get(p + "linear_out.weight"), get(p + "linear_out.bias"), o), fsmn);
  };

  auto sanm_layer = [&](ggml_context* c, const std::string& p, ggml_tensor* x, int t, bool res) -> ggml_tensor* {
    auto r = x;
    auto h = lnorm(c, x, get(p + "norm1.weight"), get(p + "norm1.bias"));
    auto sa = sanm_attn(c, p + "self_attn.", h, t);
    x = res ? ggml_add(c, r, sa) : sa;
    r = x;
    h = lnorm(c, x, get(p + "norm2.weight"), get(p + "norm2.bias"));
    h = lin(c, get(p + "feed_forward.w_1.weight"), get(p + "feed_forward.w_1.bias"), h);
    h = ggml_relu(c, h);
    h = lin(c, get(p + "feed_forward.w_2.weight"), get(p + "feed_forward.w_2.bias"), h);
    return ggml_add(c, r, h);
  };

  ggml_backend_t backend = ggml_backend_cpu_init();
  ggml_init_params cp = {static_cast<size_t>(1024) * 1024 * 1024, nullptr, true};
  ggml_context* c = ggml_init(cp);
  ggml_tensor* x = ggml_new_tensor_2d(c, GGML_TYPE_F32, input_dim, n);
  ggml_set_input(x);
  ggml_tensor* h = sanm_layer(c, "encoder.encoders0.0.", x, n, false);
  for (int i = 0; i < config_.num_blocks - 1; i++) {
    h = sanm_layer(c, "encoder.encoders." + std::to_string(i) + ".", h, n, true);
  }
  h = lnorm(c, h, get("encoder.after_norm.weight"), get("encoder.after_norm.bias"));
  for (int i = 0; i < config_.tp_blocks; i++) {
    h = sanm_layer(c, "encoder.tp_encoders." + std::to_string(i) + ".", h, n, true);
  }
  h = lnorm(c, h, get("encoder.tp_norm.weight"), get("encoder.tp_norm.bias"));
  ggml_tensor* logits = lin(c, get("ctc.ctc_lo.weight"), get("ctc.ctc_lo.bias"), h);
  if (!error.empty()) {
    ggml_free(c);
    ggml_backend_free(backend);
    return "";
  }
  ggml_set_output(logits);
  ggml_cgraph* graph = ggml_new_graph_custom(c, 32768, false);
  ggml_build_forward_expand(graph, logits);
  ggml_gallocr_t alloc = ggml_gallocr_new(ggml_backend_cpu_buffer_type());
  ggml_gallocr_alloc_graph(alloc, graph);
  ggml_backend_tensor_set(x, input.data(), 0, ggml_nbytes(x));
  ggml_backend_cpu_set_n_threads(backend, threads > 0 ? threads : 1);
  if (ggml_backend_graph_compute(backend, graph) != GGML_STATUS_SUCCESS) {
    error = "SenseVoice graph compute failed";
    ggml_gallocr_free(alloc);
    ggml_free(c);
    ggml_backend_free(backend);
    return "";
  }

  std::vector<float> logit_values(static_cast<size_t>(v) * n);
  ggml_backend_tensor_get(logits, logit_values.data(), 0, ggml_nbytes(logits));
  std::vector<int> ids;
  int prev = -1;
  for (int frame = 0; frame < n; frame++) {
    const float* col = &logit_values[static_cast<size_t>(frame) * v];
    int argmax = 0;
    float best = col[0];
    for (int token = 1; token < v; token++) {
      if (col[token] > best) {
        best = col[token];
        argmax = token;
      }
    }
    if (argmax != prev && argmax != config_.blank) ids.push_back(argmax);
    prev = argmax;
  }

  ggml_gallocr_free(alloc);
  ggml_free(c);
  ggml_backend_free(backend);
  return Detok(ids, vocab_);
}

}  // namespace vibetype
