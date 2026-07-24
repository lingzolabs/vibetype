#include "../qwen_engine.h"

// Stub implementation of QwenEngine for unit tests.
// Does NOT link llama.cpp. All methods are no-ops or return empty/false.

namespace vibetype {

QwenEngine::~QwenEngine() = default;

void QwenEngine::StartAsync(const std::string&, const std::string&, int, int) {}

bool QwenEngine::Ready() const { return false; }

QwenEngine::State QwenEngine::GetState() const { return State::kIdle; }

std::string QwenEngine::GetStateMessage() const { return "stub"; }

std::string QwenEngine::Polish(const std::string&, const std::string&,
                               const std::string&, int, int, int) {
  return "";
}

}  // namespace vibetype
