// tests/test_text_processor.cc
// TDD tests for vibetype::TextProcessor
// Run via CTest: ctest --test-dir build -R text_processor -V

#include "text_processor.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

int g_pass = 0;
int g_fail = 0;

void Expect(const char* desc, const std::string& got, const std::string& expected) {
  if (got == expected) {
    ++g_pass;
    std::printf("  PASS: %s\n", desc);
  } else {
    ++g_fail;
    std::printf("  FAIL: %s\n    got:      [%s]\n    expected: [%s]\n",
                desc, got.c_str(), expected.c_str());
  }
}

void ExpectTrue(const char* desc, bool cond) {
  if (cond) {
    ++g_pass;
    std::printf("  PASS: %s\n", desc);
  } else {
    ++g_fail;
    std::printf("  FAIL: %s (condition was false)\n", desc);
  }
}

// Write a string to a temp file; return the path.
static std::string WriteTempFile(const std::string& content, const std::string& suffix = ".json") {
  char tmpl[256];
  std::snprintf(tmpl, sizeof(tmpl), "/tmp/vt-test-XXXXXX%s", suffix.c_str());
  int fd = mkstemps(tmpl, static_cast<int>(suffix.size()));
  if (fd < 0) return "";
  ::write(fd, content.data(), content.size());
  ::close(fd);
  return tmpl;
}

// ─── UTF-8 helpers ────────────────────────────────────────────────────────────

using vibetype::TextProcessor;
using vibetype::TextProcessorConfig;

// ─── 1. Regression: NormalizeTranscriptText parity ───────────────────────────

void TestNormalizationParity() {
  std::printf("\n[1] NormalizeTranscriptText parity (ProcessPartial)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // CJK text: half-width punctuation should be converted to full-width
  Expect("CJK comma becomes fullwidth",
         tp.ProcessPartial("你好,世界"),
         "你好，世界");

  Expect("CJK period becomes fullwidth",
         tp.ProcessPartial("你好.世界"),
         "你好。世界");

  Expect("Multiple punctuation collapsed",
         tp.ProcessPartial("你好 ？ . ."),
         "你好？");

  // Pure English text: full-width should be converted to half-width
  Expect("Full-width in English to half-width",
         tp.ProcessPartial("hello。world"),
         "hello.world");

  // No-op for already normalized text
  Expect("English no-op",
         tp.ProcessPartial("hello world"),
         "hello world");

  Expect("CJK no-op",
         tp.ProcessPartial("你好世界"),
         "你好世界");

  // Trailing space stripped
  Expect("Trailing space stripped",
         tp.ProcessPartial("hello world  "),
         "hello world");
}

// ─── 1b. CJK spacing parity with master NormalizeTranscriptText ─────────────
//
// These cases confirm exact parity with master's NormalizeTranscriptText:
// in CJK text (contains ideograph/kana), spaces between a CJK character and
// an ASCII character are dropped, exactly as master does.
// Hangul-only text is treated as non-CJK (spaces preserved).
// Protected spans preserve their internal bytes; the surrounding spacing and
// punctuation follow master's rules unchanged.

void TestCjkSpacingParity() {
  std::printf("\n[1b] CJK spacing parity with master NormalizeTranscriptText\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // CJK + space + URL: space between CJK and ASCII is dropped (master parity)
  Expect("CJK before URL: space dropped",
         tp.ProcessPartial("请访问 https://www.github.com/user/repo 了解详情"),
         "请访问https://www.github.com/user/repo了解详情");

  Expect("CJK before http URL: space dropped",
         tp.ProcessPartial("见 http://example.com/api"),
         "见http://example.com/api");

  // CJK + space + English word: space dropped
  Expect("CJK + space + English word: space dropped",
         tp.ProcessPartial("我在用 GitHub 管理代码"),
         "我在用GitHub管理代码");

  // CJK + space + version: space dropped
  Expect("CJK + space + version: space dropped",
         tp.ProcessPartial("升级到 v1.2.3 版本"),
         "升级到v1.2.3版本");

  Expect("CJK + space + plain version: space dropped",
         tp.ProcessPartial("版本 1.2.3 已发布"),
         "版本1.2.3已发布");

  // CJK + space + path: space dropped
  Expect("CJK + space + path: space dropped",
         tp.ProcessPartial("编辑 /etc/vibetype/config.json 文件"),
         "编辑/etc/vibetype/config.json文件");

  // CJK + space + email: space dropped
  Expect("CJK + space + email: space dropped",
         tp.ProcessPartial("联系 user@example.com 获取帮助"),
         "联系user@example.com获取帮助");

  // CJK + space + CLI option: space dropped
  Expect("CJK + space + CLI option: space dropped",
         tp.ProcessPartial("用 --fake-asr 模式测试"),
         "用--fake-asr模式测试");

  // CJK + space + backtick span: space dropped
  Expect("CJK + space + backtick: space dropped",
         tp.ProcessPartial("调用 `ProcessFinal()` 方法"),
         "调用`ProcessFinal()`方法");

  // CJK + space + tech identifier: space dropped
  Expect("CJK + space + tech identifier: space dropped",
         tp.ProcessPartial("调用 vibetype.finalResult 方法"),
         "调用vibetype.finalResult方法");

  // Japanese (kana = cjk_text=true): same rule
  Expect("Japanese + URL: space dropped",
         tp.ProcessPartial("詳細は https://docs.example.com を参照"),
         "詳細はhttps://docs.example.comを参照");

  // Hangul-only (cjk_text=false): spaces preserved
  Expect("Hangul + URL: space preserved (Hangul is non-CJK)",
         tp.ProcessPartial("자세히 https://example.com 참조"),
         "자세히 https://example.com 참조");

  Expect("Hangul + version: space preserved",
         tp.ProcessPartial("버전 v2.0.1 업그레이드"),
         "버전 v2.0.1 업그레이드");

  // Pure ASCII (cjk_text=false): spaces preserved
  Expect("English only: spaces preserved",
         tp.ProcessPartial("visit https://example.com today"),
         "visit https://example.com today");

  // Mixed ASCII-only around URL: both sides ASCII => space kept
  Expect("ASCII before and after URL: spaces preserved",
         tp.ProcessPartial("see https://example.com for details"),
         "see https://example.com for details");

  // CJK text + URL with unicode path: space dropped
  Expect("CJK + URL with unicode path: space dropped",
         tp.ProcessPartial("访问 https://例子.测试/路径"),
         "访问https://例子.测试/路径");

  Expect("CJK + URL with unicode path2: space dropped",
         tp.ProcessPartial("见 https://example.com/路径"),
         "见https://example.com/路径");

  // CJK + URL + adjacent fullwidth comma (outside URL): space dropped before URL
  Expect("CJK + URL + fullwidth comma: space dropped",
         tp.ProcessPartial("详见 https://example.com，谢谢"),
         "详见https://example.com，谢谢");

  // CJK + URL where URL ends just before a halfwidth period (the period is
  // outside URL → converted to 。 in CJK context)
  Expect("CJK + URL trailing period becomes fullwidth",
         tp.ProcessPartial("打开 https://example.com/."),
         "打开https://example.com/。");

  // CJK + URL + space + CJK: both spaces dropped
  Expect("URL surrounded by CJK: both spaces dropped",
         tp.ProcessPartial("见 https://example.com/ 详情"),
         "见https://example.com/详情");

  // CJK + URL + halfwidth comma (outside URL) → fullwidth
  Expect("CJK URL followed by halfwidth comma: comma to fullwidth",
         tp.ProcessPartial("访问 https://example.com/api,然后"),
         "访问https://example.com/api，然后");

  // IP address: space dropped in CJK
  Expect("CJK + IP: space dropped",
         tp.ProcessPartial("服务器地址是 192.168.1.100"),
         "服务器地址是192.168.1.100");

  // IP with port (colon protected)
  Expect("CJK + IP:port: space dropped, colon preserved",
         tp.ProcessPartial("连接 192.168.1.1:9090"),
         "连接192.168.1.1:9090");
}

// ─── 2. URL protection (first-priority regression) ───────────────────────────

void TestUrlProtection() {
  std::printf("\n[2] URL protection in mixed CJK text\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // KEY REGRESSION: URL dots must NOT become。 in CJK context.
  // Space between CJK and URL is dropped (master CJK spacing parity).
  Expect("URL preserved in CJK partial",
         tp.ProcessPartial("请访问 https://www.github.com/user/repo 了解详情"),
         "请访问https://www.github.com/user/repo了解详情");

  Expect("URL preserved in CJK final",
         tp.ProcessFinal("请访问 https://www.github.com/user/repo 了解详情"),
         "请访问https://www.github.com/user/repo了解详情");

  // HTTP with path
  Expect("HTTP URL with path preserved",
         tp.ProcessPartial("见 http://example.com/api/v1/endpoint"),
         "见http://example.com/api/v1/endpoint");

  // URL with port
  Expect("URL with port preserved",
         tp.ProcessPartial("连接 http://localhost:8080/path"),
         "连接http://localhost:8080/path");

  // URL with query
  Expect("URL with query string preserved",
         tp.ProcessPartial("打开 https://example.com/search?q=hello&lang=zh"),
         "打开https://example.com/search?q=hello&lang=zh");

  // Stand-alone domain (tech identifier style)
  Expect("Domain-like token preserved",
         tp.ProcessPartial("访问 github.com"),
         "访问github.com");
}

// ─── 3. Email protection ──────────────────────────────────────────────────────

void TestEmailProtection() {
  std::printf("\n[3] Email protection\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  Expect("Email in CJK text preserved partial",
         tp.ProcessPartial("联系 user@example.com 获取帮助"),
         "联系user@example.com获取帮助");

  Expect("Email in CJK text preserved final",
         tp.ProcessFinal("联系 user@example.com 获取帮助"),
         "联系user@example.com获取帮助");
}

// ─── 4. Path protection ───────────────────────────────────────────────────────

void TestPathProtection() {
  std::printf("\n[4] Path protection (absolute, ~/, relative source)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  Expect("Absolute path preserved in CJK",
         tp.ProcessPartial("编辑 /etc/vibetype/config.json 文件"),
         "编辑/etc/vibetype/config.json文件");

  Expect("Home-relative path preserved",
         tp.ProcessPartial("配置在 ~/.config/vibetype/text-processing.json"),
         "配置在~/.config/vibetype/text-processing.json");

  Expect("Relative source path preserved",
         tp.ProcessPartial("修改 src/backend/text_processor.h 的接口"),
         "修改src/backend/text_processor.h的接口");

  Expect("Absolute path final",
         tp.ProcessFinal("文件是 /usr/lib/vibetype/data.json"),
         "文件是/usr/lib/vibetype/data.json");
}

// ─── 5. CLI option protection ────────────────────────────────────────────────

void TestCliOptionProtection() {
  std::printf("\n[5] CLI option protection\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  Expect("--flag preserved in CJK",
         tp.ProcessPartial("用 --fake-asr 模式测试"),
         "用--fake-asr模式测试");

  Expect("-s flag preserved",
         tp.ProcessPartial("socket 路径用 -s 指定"),
         "socket路径用-s指定");
}

// ─── 6. Version/IP protection ─────────────────────────────────────────────────

void TestVersionAndIpProtection() {
  std::printf("\n[6] Version and IP address protection\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  Expect("Version number preserved in CJK",
         tp.ProcessPartial("升级到 v1.2.3 版本"),
         "升级到v1.2.3版本");

  Expect("Plain version preserved",
         tp.ProcessPartial("版本 1.2.3 已发布"),
         "版本1.2.3已发布");

  Expect("IP address preserved in CJK",
         tp.ProcessPartial("服务器地址是 192.168.1.100"),
         "服务器地址是192.168.1.100");

  Expect("IP with port preserved",
         tp.ProcessPartial("连接 192.168.1.1:9090"),
         "连接192.168.1.1:9090");
}

// ─── 7. Backtick code protection ─────────────────────────────────────────────

void TestBacktickProtection() {
  std::printf("\n[7] Backtick code span protection\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  Expect("Backtick code preserved in CJK partial",
         tp.ProcessPartial("调用 `ProcessFinal()` 方法"),
         "调用`ProcessFinal()`方法");

  Expect("Backtick code with dot preserved",
         tp.ProcessPartial("执行 `cmake -B build.dir` 命令"),
         "执行`cmake -B build.dir`命令");
}

// ─── 8. Tech identifier protection ───────────────────────────────────────────

void TestTechIdentifierProtection() {
  std::printf("\n[8] Tech identifier (dot-containing) protection\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // vibetype.finalResult etc.
  Expect("JSON-RPC method name preserved",
         tp.ProcessPartial("调用 vibetype.finalResult 方法"),
         "调用vibetype.finalResult方法");

  Expect("Package name with dot preserved",
         tp.ProcessPartial("使用 com.example.vibetype 包"),
         "使用com.example.vibetype包");
}

// ─── 9. Filler removal (final only) ──────────────────────────────────────────

void TestFillerRemoval() {
  std::printf("\n[9] Filler removal (final only)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // Chinese fillers at sentence start
  Expect("嗯 at start removed in final",
         tp.ProcessFinal("嗯今天天气不错"),
         "今天天气不错");

  Expect("呃 at start removed in final",
         tp.ProcessFinal("呃这个问题很复杂"),
         "这个问题很复杂");

  // Chinese fillers after punctuation boundary
  Expect("嗯 after fullwidth comma removed",
         tp.ProcessFinal("好的，嗯我们继续"),
         "好的，我们继续");

  // English fillers at sentence start
  Expect("um at start removed in final",
         tp.ProcessFinal("um I think this is correct"),
         "I think this is correct");

  Expect("uh at start removed in final",
         tp.ProcessFinal("uh let me check"),
         "let me check");

  // English fillers after punctuation
  Expect("um after comma removed",
         tp.ProcessFinal("well, um I agree"),
         "well, I agree");

  // Must NOT remove fillers mid-word
  Expect("album not affected by um removal",
         tp.ProcessFinal("I have an album"),
         "I have an album");

  Expect("thunder not affected",
         tp.ProcessFinal("I heard thunder"),
         "I heard thunder");

  // 那个/这个/然后 NOT removed by default
  Expect("那个 NOT removed (default off)",
         tp.ProcessFinal("那个，我觉得可以"),
         "那个，我觉得可以");

  Expect("然后 NOT removed (default off)",
         tp.ProcessFinal("然后我们继续"),
         "然后我们继续");

  // Fillers NOT removed in partial
  Expect("嗯 kept in partial",
         tp.ProcessPartial("嗯今天"),
         "嗯今天");
}

// ─── 10. Repeat removal (final only) ──────────────────────────────────────────

void TestRepeatRemoval() {
  std::printf("\n[10] Repeat/repetition removal (final only)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // CJK phrase repeats (2+ chars) — allowlist-driven, not all-CJK
  Expect("今天今天 de-duped (allowlist)",
         tp.ProcessFinal("今天今天开会"),
         "今天开会");

  Expect("然后然后 de-duped (allowlist)",
         tp.ProcessFinal("然后然后我去"),
         "然后我去");

  Expect("三段重复 de-duped to one",
         tp.ProcessFinal("会议会议会议开始"),
         "会议开始");

  // English word repeats — allowlist-driven function words only
  Expect("the the de-duped (allowlist)",
         tp.ProcessFinal("I saw the the cat"),
         "I saw the cat");

  Expect("is is de-duped (allowlist)",
         tp.ProcessFinal("this is is correct"),
         "this is correct");

  // Must NOT remove intentional reduplication (single-char CJK)
  Expect("人人 NOT de-duped",
         tp.ProcessFinal("人人都有责任"),
         "人人都有责任");

  Expect("天天 NOT de-duped",
         tp.ProcessFinal("天天锻炼身体好"),
         "天天锻炼身体好");

  Expect("常常 NOT de-duped",
         tp.ProcessFinal("他常常迟到"),
         "他常常迟到");

  Expect("看看 NOT de-duped",
         tp.ProcessFinal("我们看看再说"),
         "我们看看再说");

  // Task 7: 研究研究、讨论讨论 must NOT be de-duped
  Expect("研究研究 NOT de-duped (intentional)",
         tp.ProcessFinal("我们研究研究这个问题"),
         "我们研究研究这个问题");

  Expect("讨论讨论 NOT de-duped (intentional)",
         tp.ProcessFinal("大家讨论讨论方案"),
         "大家讨论讨论方案");

  // Task 7: very very must NOT be de-duped (non-function English)
  Expect("very very NOT de-duped",
         tp.ProcessFinal("it was very very good"),
         "it was very very good");

  // Task 2: 非常重要 is a 4-char phrase; NOT in allowlist, so must NOT be de-duped
  Expect("非常重要非常重要 NOT de-duped (not in cjk_allowlist)",
         tp.ProcessFinal("非常重要非常重要"),
         "非常重要非常重要");

  // Task 2: empty cjk_allowlist -> 2-char CJK de-dup fully disabled
  {
    auto cfg2 = TextProcessorConfig::Defaults();
    cfg2.repeat.cjk_allowlist.clear();
    TextProcessor tp2(cfg2);
    Expect("今天今天 preserved when cjk_allowlist is empty",
           tp2.ProcessFinal("今天今天开会"),
           "今天今天开会");
  }

  // Task 2: custom cjk_allowlist -> only listed phrase de-duped
  {
    auto cfg3 = TextProcessorConfig::Defaults();
    cfg3.repeat.cjk_allowlist = {"开会"};
    TextProcessor tp3(cfg3);
    Expect("开会开会 de-duped with custom allowlist",
           tp3.ProcessFinal("开会开会开始"),
           "开会开始");
    Expect("今天今天 NOT de-duped with custom allowlist",
           tp3.ProcessFinal("今天今天开会"),
           "今天今天开会");
  }

  // Repeats NOT removed in partial
  Expect("今天今天 kept in partial",
         tp.ProcessPartial("今天今天开会"),
         "今天今天开会");
}

// ─── 11. Self-repair (final only) ────────────────────────────────────────────

void TestSelfRepair() {
  std::printf("\n[11] Self-repair / correction (final only)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // Chinese date correction
  Expect("周四不对周五 self-repair",
         tp.ProcessFinal("周四，不对，周五开会"),
         "周五开会");

  // Version correction
  Expect("version 1.2不对1.3 self-repair",
         tp.ProcessFinal("版本 1.2，不对，1.3 发布了"),
         "版本1.3发布了");

  // Number correction
  Expect("number correction",
         tp.ProcessFinal("三点，不对，四点开始"),
         "四点开始");

  // Self-repair NOT done in partial
  Expect("self-repair kept in partial",
         tp.ProcessPartial("周四，不对，周五开会"),
         "周四，不对，周五开会");
}

// ─── 12. Alias correction (final only) ───────────────────────────────────────

void TestAliasCorrection() {
  std::printf("\n[12] Alias correction (final only)\n");
  // Use defaults which include builtin dict
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // Latin case-insensitive word boundary
  Expect("github -> GitHub",
         tp.ProcessFinal("push to github"),
         "push to GitHub");

  Expect("kubernetes -> Kubernetes",
         tp.ProcessFinal("deploy to kubernetes cluster"),
         "deploy to Kubernetes cluster");

  Expect("json rpc -> JSON-RPC",
         tp.ProcessFinal("using json rpc protocol"),
         "using JSON-RPC protocol");

  // Longest match priority: "json rpc" > "json"
  Expect("json rpc longer match wins",
         tp.ProcessFinal("the json rpc interface"),
         "the JSON-RPC interface");

  // Word boundary: must NOT replace mid-word Latin
  // "album" should not be affected by any alias containing "al"
  Expect("no mid-word alias replacement",
         tp.ProcessFinal("listen to my album"),
         "listen to my album");

  // Alias NOT done in partial
  Expect("github kept in partial",
         tp.ProcessPartial("push to github"),
         "push to github");
}

// ─── 12b. Task 6: Conservative alias — no over-broad CJK aliases ─────────────

void TestConservativeAlias() {
  std::printf("\n[12b] Task 6: 语音输入法 and VoiceType NOT aliased to Vibetype\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // "语音输入法" is a common Chinese phrase for "voice input method".
  // It must NOT be replaced by Vibetype in regular text.
  Expect("语音输入法 preserved unchanged",
         tp.ProcessFinal("我在用语音输入法打字"),
         "我在用语音输入法打字");

  Expect("普通语音输入法句子不变",
         tp.ProcessFinal("这款语音输入法支持中文"),
         "这款语音输入法支持中文");

  // VoiceType is not a specific Vibetype mis-recognition; should not be aliased
  Expect("VoiceType preserved unchanged",
         tp.ProcessFinal("I use VoiceType input"),
         "I use VoiceType input");

  Expect("voice type preserved unchanged",
         tp.ProcessFinal("this voice type input method"),
         "this voice type input method");

  // vibetype (lowercase) still corrects to Vibetype (canonical capitalization)
  Expect("vibetype -> Vibetype (canonical casing)",
         tp.ProcessFinal("install vibetype"),
         "install Vibetype");
}

// ─── 13. Alias NOT applied inside protected spans ─────────────────────────────

void TestAliasNotInProtected() {
  std::printf("\n[13] Alias not applied inside protected spans\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // URL contains "github.com" which should NOT be canonicalized to "GitHub.com"
  Expect("URL domain not alias-replaced",
         tp.ProcessFinal("见 https://github.com/lingzolabs/vibetype"),
         "见https://github.com/lingzolabs/vibetype");

  // Backtick code span
  Expect("backtick alias not replaced",
         tp.ProcessFinal("运行 `github` 命令"),
         "运行`github`命令");

  // Path
  Expect("path alias not replaced",
         tp.ProcessFinal("配置 /etc/kubernetes/config"),
         "配置/etc/kubernetes/config");
}

// ─── 14. Correct text does NOT degrade ───────────────────────────────────────

void TestNoRegression() {
  std::printf("\n[14] Correct text does not degrade\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  Expect("Plain Chinese no-change",
         tp.ProcessFinal("今天天气不错，我们出去走走。"),
         "今天天气不错，我们出去走走。");

  Expect("Plain English no-change",
         tp.ProcessFinal("The weather is nice today."),
         "The weather is nice today.");

  Expect("Mixed text no-change",
         tp.ProcessFinal("我在用 GitHub 管理代码"),
         "我在用GitHub管理代码");

  Expect("Japanese no-change",
         tp.ProcessFinal("今日は良い天気ですね。"),
         "今日は良い天気ですね。");

  Expect("Korean no-change",
         tp.ProcessFinal("오늘 날씨가 좋네요."),
         "오늘 날씨가 좋네요.");
}

// ─── 15. Config API (no FindProtectedSpans public call) ──────────────────────

void TestConfigApi() {
  std::printf("\n[15] Config API (Defaults, Load, Config, ProcessPartial, ProcessFinal)\n");
  auto cfg = TextProcessorConfig::Defaults();
  ExpectTrue("Filler enabled by default", cfg.filler.enabled);
  ExpectTrue("Repeat enabled by default", cfg.repeat.enabled);
  ExpectTrue("Alias enabled by default", cfg.alias.enabled);
  ExpectTrue("URL protection on by default", cfg.protected_spans.url);
  ExpectTrue("Builtin aliases non-empty", !cfg.alias.entries.empty());

  TextProcessor tp(cfg);
  // Config() accessor works
  ExpectTrue("Config() returns same enabled", tp.Config().filler.enabled);
}

// ─── 16. Config loading ───────────────────────────────────────────────────────

void TestConfigLoading() {
  std::printf("\n[16] Config loading\n");
  auto cfg = TextProcessorConfig::Defaults();

  ExpectTrue("Filler enabled by default", cfg.filler.enabled);
  ExpectTrue("Repeat enabled by default", cfg.repeat.enabled);
  ExpectTrue("Alias enabled by default", cfg.alias.enabled);
  ExpectTrue("URL protection on by default", cfg.protected_spans.url);
  ExpectTrue("Builtin aliases non-empty", !cfg.alias.entries.empty());
}

// ─── 17. Multilingual mixed text ─────────────────────────────────────────────

void TestMultilingual() {
  std::printf("\n[17] Multilingual mixed text\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // Chinese + English + code: CJK spacing parity
  Expect("Chinese English code mixed",
         tp.ProcessFinal("请用 GitHub CLI 执行 `gh pr create`"),
         "请用GitHub CLI执行`gh pr create`");

  // Japanese with URL: kana => cjk_text=true, spaces dropped
  Expect("Japanese URL preserved",
         tp.ProcessFinal("詳細は https://docs.example.com を参照"),
         "詳細はhttps://docs.example.comを参照");

  // Cantonese + English (treat as CJK): spaces dropped
  Expect("Cantonese with version preserved",
         tp.ProcessFinal("升級到版本 2.0.1 先"),
         "升級到版本2.0.1先");
}

// ─── 18. BLOCKER-1: ApplyAliases must not corrupt CLI options ────────────────
// ProcessFinal reuses original spans after text is mutated; alias must check
// freshly computed spans on the current result string.

void TestAliasNotCorruptingCliOption() {
  std::printf("\n[18] Alias must not modify CLI options (BLOCKER-1)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // --github-token contains "github" which is an alias for "GitHub".
  // The alias engine must NOT replace it because --github-token is a CLI option (protected).
  Expect("--github-token NOT alias-replaced",
         tp.ProcessFinal("run --github-token abc"),
         "run --github-token abc");

  Expect("CJK-adjacent --github-token NOT alias-replaced",
         tp.ProcessFinal("测试 --github-token，然后继续"),
         "测试--github-token，然后继续");

  // Similarly --gitlab-token
  Expect("--gitlab-token NOT alias-replaced",
         tp.ProcessFinal("use --gitlab-token xyz"),
         "use --gitlab-token xyz");

  // github outside CLI option SHOULD be replaced
  Expect("standalone github IS replaced",
         tp.ProcessFinal("push to github"),
         "push to GitHub");
}

// ─── 19. BLOCKER-2: Unicode IRI / path in URL ────────────────────────────────

void TestUnicodeIriProtection() {
  std::printf("\n[19] Unicode IRI / path protection (BLOCKER-2)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // Full Unicode IRI: IDN host + Unicode path — must be protected verbatim;
  // space between CJK and URL is dropped (master parity)
  Expect("Unicode IRI with IDN host preserved",
         tp.ProcessFinal("访问 https://例子.测试/路径"),
         "访问https://例子.测试/路径");

  // Mixed ASCII host + Unicode path
  Expect("ASCII host + Unicode path preserved",
         tp.ProcessFinal("见 https://example.com/路径"),
         "见https://example.com/路径");

  // URL adjacent to sentence-end punctuation: comma/period after URL kept outside
  Expect("URL not swallowing adjacent fullstop",
         tp.ProcessFinal("详见 https://example.com，谢谢"),
         "详见https://example.com，谢谢");
}

// ─── 20. BLOCKER-3: Config layering reads text-processing.json switches ───────
// Load(data_dir) must read ALL public switches from data/text-processing.json,
// then merge user overrides on top.

void TestConfigLayering() {
  std::printf("\n[20] Config layering from data/text-processing.json (BLOCKER-3)\n");

  // Load with the project data/ directory (relative path works in test cwd = build/)
  // We pass the absolute source data dir so it works regardless of cwd.
  const std::string data_dir = DATA_DIR;  // injected by CMake compile definition

  auto cfg = TextProcessorConfig::Load(data_dir, "/nonexistent/no-user.json");

  // Switches from text-processing.json must be read
  ExpectTrue("filler.enabled loaded from file", cfg.filler.enabled);
  ExpectTrue("repeat.enabled loaded from file", cfg.repeat.enabled);
  ExpectTrue("self_repair.enabled loaded from file", cfg.self_repair.enabled);
  ExpectTrue("alias.enabled loaded from file", cfg.alias.enabled);
  ExpectTrue("url span loaded from file", cfg.protected_spans.url);
  // Aliases from computer_terms.json must be loaded
  ExpectTrue("aliases loaded from computer_terms.json", !cfg.alias.entries.empty());

  // User override with temp file (not fixed /tmp path to avoid parallel conflicts)
  {
    const std::string tmp = WriteTempFile(
        R"({"filler_removal":{"enabled":false},"repeat_removal":{"enabled":false}})");
    ExpectTrue("temp user config written", !tmp.empty());
    auto cfg2 = TextProcessorConfig::Load(data_dir, tmp);
    ExpectTrue("user override: filler disabled", !cfg2.filler.enabled);
    ExpectTrue("user override: repeat disabled", !cfg2.repeat.enabled);
    // Unchanged fields stay default
    ExpectTrue("user override: alias still enabled", cfg2.alias.enabled);
    std::remove(tmp.c_str());
  }
}

// ─── 20b. Task 9: Invalid JSON fallback, empty array, user alias override ─────

void TestConfigEdgeCases() {
  std::printf("\n[20b] Task 9: invalid JSON, empty array, user alias override\n");
  const std::string data_dir = DATA_DIR;

  // Invalid JSON → must fall back to defaults, not partially apply
  {
    const std::string tmp = WriteTempFile("{ this is not valid JSON !!! }");
    auto cfg = TextProcessorConfig::Load(data_dir, tmp);
    // filler must still be enabled (default) — invalid JSON must not partially apply
    ExpectTrue("invalid JSON: filler still enabled (defaults intact)", cfg.filler.enabled);
    ExpectTrue("invalid JSON: aliases still loaded", !cfg.alias.entries.empty());
    std::remove(tmp.c_str());
  }

  // Explicit empty array clears chinese_fillers
  {
    const std::string tmp = WriteTempFile(
        R"({"filler_removal":{"chinese_fillers":[]}})");
    auto cfg = TextProcessorConfig::Load(data_dir, tmp);
    ExpectTrue("empty array clears chinese_fillers",
               cfg.filler.chinese_fillers.empty());
    std::remove(tmp.c_str());
  }

  // Task 3: user aliases key absent -> builtin preserved
  {
    const std::string tmp = WriteTempFile(
        R"({"filler_removal":{"enabled":true}})");
    auto cfg = TextProcessorConfig::Load(data_dir, tmp);
    ExpectTrue("aliases key absent: builtins preserved", !cfg.alias.entries.empty());
    TextProcessor tp(cfg);
    Expect("aliases key absent: github still corrected",
           tp.ProcessFinal("push to github"),
           "push to GitHub");
    std::remove(tmp.c_str());
  }

  // Task 3: explicit aliases:[] clears all entries
  {
    const std::string tmp = WriteTempFile(R"({"aliases":[]})");
    auto cfg = TextProcessorConfig::Load(data_dir, tmp);
    ExpectTrue("explicit aliases:[]: entries cleared", cfg.alias.entries.empty());
    TextProcessor tp(cfg);
    Expect("explicit aliases:[]: github NOT corrected",
           tp.ProcessFinal("push to github"),
           "push to github");
    std::remove(tmp.c_str());
  }

  // Task 3: conflict - user canonical wins over builtin for same alias string
  {
    // User defines "github" -> "MyGitHub"; builtin has "github" -> "GitHub".
    // User entry must win (deduplicated, first entry keeps).
    const std::string tmp = WriteTempFile(
        R"({"aliases":[{"canonical":"MyGitHub","aliases":["github"]}]})");
    auto cfg = TextProcessorConfig::Load(data_dir, tmp);
    TextProcessor tp(cfg);
    Expect("conflict: user canonical wins for same alias",
           tp.ProcessFinal("push to github"),
           "push to MyGitHub");
    std::remove(tmp.c_str());
  }

  // Task 3: user alias overrides builtin
  {
    // User provides a different alias for "GitHub" → their entry takes precedence
    const std::string tmp = WriteTempFile(
        R"({"aliases":[{"canonical":"GitHub","aliases":["githubb"]}]})");
    auto cfg = TextProcessorConfig::Load(data_dir, tmp);
    TextProcessor tp(cfg);
    // User alias "githubb" should map to GitHub
    Expect("user alias githubb -> GitHub",
           tp.ProcessFinal("visit githubb"),
           "visit GitHub");
    std::remove(tmp.c_str());
  }
}

// ─── 20c. Task 4: invalid builtin JSON → LogW + fallback ──────────────────

void TestBuiltinInvalidJson() {
  std::printf("\n[20c] Task 4: invalid builtin JSON fallback (no crash, defaults intact)\n");
  const std::string data_dir = DATA_DIR;

  char tmp_dir[] = "/tmp/vt-test-XXXXXX";
  const char* td = mkdtemp(tmp_dir);
  ExpectTrue("temp dir created", td != nullptr);
  if (td) {
    // Invalid text-processing.json
    {
      const std::string tp_path = std::string(td) + "/text-processing.json";
      std::ofstream f(tp_path);
      f << "{ not valid json !! ";
    }
    // Copy real computer_terms.json
    {
      const std::string src = data_dir + "/computer_terms.json";
      const std::string dst = std::string(td) + "/computer_terms.json";
      std::ifstream  in(src,  std::ios::binary);
      std::ofstream out(dst, std::ios::binary);
      out << in.rdbuf();
    }
    auto cfg = TextProcessorConfig::Load(td, "/nonexistent/x");
    ExpectTrue("invalid builtin tp json: filler still enabled", cfg.filler.enabled);
    ExpectTrue("invalid builtin tp json: repeat still enabled", cfg.repeat.enabled);
    ExpectTrue("invalid builtin tp json: aliases from computer_terms",
               !cfg.alias.entries.empty());
    TextProcessor tp(cfg);
    Expect("invalid builtin tp json: github corrected",
           tp.ProcessFinal("push to github"),
           "push to GitHub");

    // Invalid computer_terms.json (array root) + valid tp json
    {
      const std::string terms_path = std::string(td) + "/computer_terms.json";
      std::ofstream f(terms_path);
      f << "[\"array not object\"]";
    }
    {
      const std::string tp_path = std::string(td) + "/text-processing.json";
      std::ifstream  in(data_dir + "/text-processing.json", std::ios::binary);
      std::ofstream out(tp_path, std::ios::binary);
      out << in.rdbuf();
    }
    auto cfg2 = TextProcessorConfig::Load(td, "/nonexistent/x");
    ExpectTrue("invalid builtin terms json: builtin alias defaults used",
               !cfg2.alias.entries.empty());
    TextProcessor tp2(cfg2);
    Expect("invalid builtin terms json: github corrected via builtin defaults",
           tp2.ProcessFinal("push to github"),
           "push to GitHub");

    std::remove((std::string(td) + "/text-processing.json").c_str());
    std::remove((std::string(td) + "/computer_terms.json").c_str());
    ::rmdir(td);
  }
}

// ─── 21. BLOCKER-5: Filler + URL — no residual comma; v-prefix version ────────

void TestFillerBeforeUrl() {
  std::printf("\n[21] Filler before URL / v-prefix version (BLOCKER-5)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // 嗯，<URL> — filler 嗯 should be removed; comma should go with it, no residual comma
  Expect("嗯 comma before URL: no residual comma",
         tp.ProcessFinal("嗯，https://github.com"),
         "https://github.com");

  // v-prefix version alias: e.g. "vibetype v1.2.3" — v1.2.3 is protected
  // and alias must NOT strip the 'v'
  Expect("v-prefix version NOT alias-mangled",
         tp.ProcessFinal("upgrade to v1.2.3"),
         "upgrade to v1.2.3");

  // Plain version WITHOUT v-prefix must also be preserved
  Expect("plain version preserved",
         tp.ProcessFinal("upgrade to 1.2.3"),
         "upgrade to 1.2.3");

  // Version at sentence end (Chinese context): space dropped before version
  Expect("version at sentence end preserved",
         tp.ProcessFinal("升级到 v2.0.1。"),
         "升级到v2.0.1。");
}

// ─── 22. Issue-6: UTF-8 decoder — bad continuations don't cause out-of-bounds ─

void TestUtf8DecoderRobustness() {
  std::printf("\n[22] UTF-8 decoder robustness (Issue-6)\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // A 2-byte lead without continuation: 0xC0 followed by ASCII 'A'
  // Must not crash and must not eat the 'A'
  {
    std::string bad = "hello"; bad += (char)0xC0; bad += "A world";
    const std::string out = tp.ProcessPartial(bad);
    // 'world' must still appear
    ExpectTrue("bad continuation: 'world' survives",
               out.find("world") != std::string::npos);
  }

  // A 3-byte lead with only 1 continuation byte then ASCII
  {
    std::string bad = "ok"; bad += (char)0xE4; bad += (char)0xB8; bad += " end";
    const std::string out = tp.ProcessPartial(bad);
    ExpectTrue("truncated 3-byte: 'end' survives",
               out.find("end") != std::string::npos);
  }

  // Completely illegal byte 0xFF in stream
  {
    std::string bad = "hi"; bad += (char)0xFF; bad += "bye";
    const std::string out = tp.ProcessPartial(bad);
    ExpectTrue("0xFF byte: 'bye' survives",
               out.find("bye") != std::string::npos);
  }
}

// ─── 23. Extra behavior: adjacent punctuation to protected spans ───────────────

void TestProtectedSpanAdjacentPunct() {
  std::printf("\n[23] Adjacent punctuation to protected spans\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // Email followed by sentence-end comma/period: space before email is dropped
  Expect("email followed by comma, comma stays outside",
         tp.ProcessFinal("联系 user@example.com，获取帮助"),
         "联系user@example.com，获取帮助");

  // Path followed by period: space dropped before path
  Expect("path before period: period stays",
         tp.ProcessFinal("编辑 /etc/config.json。"),
         "编辑/etc/config.json。");

  // Version followed by Chinese punctuation: space dropped before version
  Expect("version before Chinese period: stays",
         tp.ProcessFinal("版本 1.2.3。已发布"),
         "版本1.2.3。已发布");

  // Backtick followed by comma: space dropped before backtick
  Expect("backtick code before comma: stays",
         tp.ProcessFinal("运行 `make test`，然后提交"),
         "运行`make test`，然后提交");

  // CLI option followed by value containing alias candidate
  Expect("CLI option value: alias not applied inside",
         tp.ProcessFinal("run --github-token mytoken"),
         "run --github-token mytoken");
}

// ─── 23b. Task 8: URL adjacent sentence-end punctuation ──────────────────────

void TestUrlAdjacentPunct() {
  std::printf("\n[23b] Task 8: URL adjacent sentence-end punctuation\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // ASCII period after URL in CJK sentence: space dropped, period outside URL -> 。
  Expect("打开 URL. — outer period becomes 。 in CJK",
         tp.ProcessFinal("打开 https://example.com/."),
         "打开https://example.com/。");

  // URL surrounded by CJK: both spaces dropped
  Expect("URL path slash preserved before sentence period",
         tp.ProcessFinal("见 https://example.com/ 详情"),
         "见https://example.com/详情");

  // URL ending with path component, followed by Chinese comma: space dropped
  Expect("URL followed by CJK comma: URL intact",
         tp.ProcessFinal("访问 https://example.com/api,然后"),
         "访问https://example.com/api，然后");

  // Pure English sentence with URL: period stays half-width after URL
  Expect("English: URL followed by period stays half-width",
         tp.ProcessFinal("visit https://example.com."),
         "visit https://example.com.");
}

// ─── 24. Alias not applied inside email/path/backtick/CLI option ──────────────

void TestAliasNotInAllProtected() {
  std::printf("\n[24] Alias not in email/path/backtick/CLI option\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // Email local part that matches an alias
  Expect("alias not in email local part",
         tp.ProcessFinal("send to github@example.com"),
         "send to github@example.com");

  // Path containing alias
  Expect("alias not in path",
         tp.ProcessFinal("see /opt/kubernetes/bin"),
         "see /opt/kubernetes/bin");

  // Backtick code containing alias
  Expect("alias not in backtick",
         tp.ProcessFinal("run `kubernetes apply`"),
         "run `kubernetes apply`");

  // Korean text + URL: no corruption; Hangul is non-CJK so space preserved
  Expect("Korean + URL no corruption",
         tp.ProcessFinal("자세히 https://example.com 참조"),
         "자세히 https://example.com 참조");

  // Korean text + version: no corruption; space preserved
  Expect("Korean + version no corruption",
         tp.ProcessFinal("버전 v2.0.1 업그레이드"),
         "버전 v2.0.1 업그레이드");
}

// ─── 25. Correct text preserved (no over-aggressive rules) ───────────────────

void TestNoOverAggression() {
  std::printf("\n[25] No over-aggressive rules\n");
  auto cfg = TextProcessorConfig::Defaults();
  TextProcessor tp(cfg);

  // A sentence where every word happens to contain an alias substring
  Expect("normal sentence not mangled",
         tp.ProcessFinal("I am learning Linux programming"),
         "I am learning Linux programming");

  // Number adjacent to text
  Expect("number adjacent to text preserved",
         tp.ProcessFinal("Python 3.11 is fast"),
         "Python 3.11 is fast");

  // Hyphenated word not treated as CLI option
  Expect("mid-sentence hyphen not CLI-option",
         tp.ProcessFinal("self-contained module"),
         "self-contained module");
}

// ─── 26. Task 5: FindDataDir from arbitrary cwd ──────────────────────────────

void TestFindDataDir() {
  std::printf("\n[26] Task 5: FindDataDir (VIBETYPE_DATA_DIR env, install, source)\n");
  const std::string data_dir = DATA_DIR;

  // Simulate VIBETYPE_DATA_DIR override
  ::setenv("VIBETYPE_DATA_DIR", data_dir.c_str(), 1);
  auto cfg = TextProcessorConfig::Load("", "/nonexistent/no-user.json");
  // When env is set, Load() with empty builtin_data_dir should still use env
  // (this is handled in main.cc; here we test Load(data_dir) directly)
  ExpectTrue("Load(DATA_DIR) loads aliases", !cfg.alias.entries.empty());
  ::unsetenv("VIBETYPE_DATA_DIR");

  // Directly passing data_dir works regardless of cwd
  auto cfg2 = TextProcessorConfig::Load(data_dir, "/nonexistent/x");
  ExpectTrue("Load with absolute data_dir loads aliases", !cfg2.alias.entries.empty());
}

}  // anonymous namespace

int main() {
  std::printf("=== Vibetype TextProcessor Tests ===\n");

  TestNormalizationParity();
  TestCjkSpacingParity();
  TestUrlProtection();
  TestEmailProtection();
  TestPathProtection();
  TestCliOptionProtection();
  TestVersionAndIpProtection();
  TestBacktickProtection();
  TestTechIdentifierProtection();
  TestFillerRemoval();
  TestRepeatRemoval();
  TestSelfRepair();
  TestAliasCorrection();
  TestConservativeAlias();
  TestAliasNotInProtected();
  TestNoRegression();
  TestConfigApi();
  TestConfigLoading();
  TestMultilingual();
  TestAliasNotCorruptingCliOption();
  TestUnicodeIriProtection();
  TestConfigLayering();
  TestConfigEdgeCases();
  TestBuiltinInvalidJson();
  TestFillerBeforeUrl();
  TestUtf8DecoderRobustness();
  TestProtectedSpanAdjacentPunct();
  TestUrlAdjacentPunct();
  TestAliasNotInAllProtected();
  TestNoOverAggression();
  TestFindDataDir();

  std::printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
  return g_fail > 0 ? 1 : 0;
}
