#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

#include "../config_manager.h"
#include "../text_processor.h"

// ── Test helpers ──────────────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

#define CHECK(cond) do { \
  if (cond) { ++g_pass; } \
  else { ++g_fail; \
    fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #cond); } \
} while(0)

#define CHECK_EQ(a, b) do { \
  const std::string _va = (a); const std::string _vb = (b); \
  if (_va == _vb) { ++g_pass; } \
  else { ++g_fail; \
    fprintf(stderr, "FAIL [%s:%d]: %s != %s\n  got: '%s'\n  exp: '%s'\n", \
      __FILE__, __LINE__, #a, #b, _va.c_str(), _vb.c_str()); } \
} while(0)

// ── Tests ─────────────────────────────────────────────────────────────────────

static void TestApplyCorrectionsBasic() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["github"] = "GitHub";
  cfg.custom_corrections["linux"] = "Linux";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("I use github on linux", cfg),
           "I use GitHub on Linux");
}

static void TestApplyCorrectionsWordBoundary() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["api"] = "API";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("rest api call", cfg), "rest API call");
}

static void TestApplyCorrectionsLongestFirst() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["github"] = "GitHub";
  cfg.custom_corrections["github actions"] = "GitHub Actions";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("use github actions today", cfg),
           "use GitHub Actions today");
}

static void TestProcessFinalNoQwen() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["docker"] = "Docker";
  cfg.enable_qwen_polish = false;

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessFinal("run docker container", cfg, nullptr),
           "run Docker container");
}

static void TestProcessFinalQwenDisabledWhenNotReady() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["kubernetes"] = "Kubernetes";
  cfg.enable_qwen_polish = true;

  // Stub QwenEngine::Ready() returns false → fallback to rule-based.
  vibetype::QwenEngine qwen;
  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessFinal("deploy kubernetes cluster", cfg, &qwen),
           "deploy Kubernetes cluster");
}

static void TestEmptyText() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["test"] = "TEST";
  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("", cfg), "");
  CHECK_EQ(tp.ProcessFinal("", cfg, nullptr), "");
}

static void TestCaseSensitiveKey() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["Github"] = "GitHub";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("I use Github daily", cfg), "I use GitHub daily");
  // lowercase "github" does NOT match key "Github".
  CHECK_EQ(tp.ProcessPartial("I use github daily", cfg), "I use github daily");
}

static void TestNoDoubleReplace() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["linux"] = "Linux";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("linux linux", cfg), "Linux Linux");
}

// ── New tests: Chinese boundary and array-format corrections ─────────────────

// Test: correction key matches inside continuous Chinese text (no spaces).
static void TestChineseBoundaryReplacement() {
  vibetype::BackendConfig cfg;
  // "github" should be corrected inside a Chinese sentence like "用github提交代码"
  cfg.custom_corrections["github"] = "GitHub";

  vibetype::TextProcessor tp;
  // Chinese text with embedded ASCII term — no spaces around "github"
  CHECK_EQ(tp.ProcessPartial("用github提交代码", cfg), "用GitHub提交代码");
  CHECK_EQ(tp.ProcessPartial("请使用github actions", cfg), "请使用GitHub actions");
}

// Test: correction key that is Chinese text matches inside Chinese sentence.
static void TestChineseKeyReplacement() {
  vibetype::BackendConfig cfg;
  // Replace a common misrecognition of a Chinese word.
  cfg.custom_corrections["的话"] = "的话";  // identity — just check no crash
  cfg.custom_corrections["机器学习"] = "机器学习（ML）";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("这是机器学习项目", cfg), "这是机器学习（ML）项目");
  CHECK_EQ(tp.ProcessPartial("机器学习和深度学习", cfg), "机器学习（ML）和深度学习");
}

// Test: array format [{from, to}] is parsed and applied.
static void TestArrayFormatCorrections() {
  // Simulate what ParseCustomCorrections does for array format by building
  // the map directly (since we test DoLoad separately).
  vibetype::BackendConfig cfg;
  // Populate corrections as if parsed from [{from:"github",to:"GitHub"}]
  cfg.custom_corrections["github"] = "GitHub";
  cfg.custom_corrections["linux"] = "Linux";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("run github on linux", cfg),
           "run GitHub on Linux");
}

// Test: empty "from" key is rejected (correction must not crash or apply).
static void TestEmptyKeyIgnored() {
  vibetype::BackendConfig cfg;
  // Even if an empty key somehow got in, it must be skipped by ApplyCorrections.
  cfg.custom_corrections[""] = "SHOULD_NOT_APPEAR";
  cfg.custom_corrections["linux"] = "Linux";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("run linux", cfg), "run Linux");
  // The empty key replacement must not corrupt output.
  const std::string result = tp.ProcessPartial("hello world", cfg);
  CHECK(result.find("SHOULD_NOT_APPEAR") == std::string::npos);
}

// Test: correction does not match a partial UTF-8 sequence boundary.
static void TestNoCorrectionMidCjk() {
  vibetype::BackendConfig cfg;
  cfg.custom_corrections["api"] = "API";

  vibetype::TextProcessor tp;
  CHECK_EQ(tp.ProcessPartial("调用api接口", cfg), "调用API接口");
  CHECK_EQ(tp.ProcessPartial("capital gain", cfg), "capital gain");
}

static void TestQwenPromptComesOnlyFromConfig() {
  vibetype::BackendConfig defaults;
  CHECK(defaults.qwen_system_prompt.empty());

  const auto root = std::filesystem::temp_directory_path() /
                    ("vibetype-prompt-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root / "data");
  std::ofstream(root / "backend.json") << R"({})";
  std::ofstream(root / "text-processing.json")
      << R"({"enable_qwen_polish":true})";
  std::ofstream(root / "data" / "backend.json") << R"({})";
  std::ofstream(root / "data" / "text-processing.json")
      << R"({"enable_qwen_polish":false,"qwen_system_prompt":"default prompt"})";
  std::ofstream(root / "data" / "computer_terms.json")
      << R"({"corrections":{}})";

  vibetype::ConfigManager manager((root / "backend.json").string(),
                                  (root / "text-processing.json").string(),
                                  (root / "data").string());
  manager.LoadInitial();
  CHECK(manager.GetConfig().enable_qwen_polish);
  CHECK_EQ(manager.GetConfig().qwen_system_prompt, "default prompt");

  std::ofstream(root / "text-processing.json")
      << R"({"enable_qwen_polish":true,"qwen_system_prompt":"user prompt"})";
  CHECK(manager.ForceReload());
  CHECK_EQ(manager.GetConfig().qwen_system_prompt, "user prompt");

  std::ofstream(root / "text-processing.json")
      << R"({"enable_qwen_polish":true,"qwen_system_prompt":""})";
  CHECK(manager.ForceReload());
  CHECK(manager.GetConfig().qwen_system_prompt.empty());

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

static void TestTextConfigCanClearBackendCorrections() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("vibetype-config-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root / "data");
  {
    std::ofstream(root / "backend.json")
        << R"({"enable_builtin_corrections":false,"custom_corrections":{"old":"new"}})";
    std::ofstream(root / "text-processing.json")
        << R"({"enable_builtin_corrections":false,"custom_corrections":[]})";
    std::ofstream(root / "data" / "computer_terms.json")
        << R"({"corrections":{}})";
  }
  vibetype::ConfigManager manager((root / "backend.json").string(),
                                  (root / "text-processing.json").string(),
                                  (root / "data").string());
  manager.LoadInitial();
  CHECK(manager.GetConfig().custom_corrections.empty());
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

int main() {
  TestApplyCorrectionsBasic();
  TestApplyCorrectionsWordBoundary();
  TestApplyCorrectionsLongestFirst();
  TestProcessFinalNoQwen();
  TestProcessFinalQwenDisabledWhenNotReady();
  TestEmptyText();
  TestCaseSensitiveKey();
  TestNoDoubleReplace();

  // New tests.
  TestChineseBoundaryReplacement();
  TestChineseKeyReplacement();
  TestArrayFormatCorrections();
  TestEmptyKeyIgnored();
  TestNoCorrectionMidCjk();
  TestQwenPromptComesOnlyFromConfig();
  TestTextConfigCanClearBackendCorrections();

  fprintf(stderr, "\nResults: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
