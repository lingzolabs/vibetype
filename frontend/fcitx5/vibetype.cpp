/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025-2027 Lingzo Labs
 */

#include "vibetype.h"

#include <fcitx-config/iniparser.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fcitx {

// ── recording animation frames (language-neutral symbols) ─────────

static constexpr std::array<const char *, 3> kRecordingFrames = {
    "🎤 ●○○",
    "🎤 ○●○",
    "🎤 ○○●",
};

static constexpr uint64_t kAnimationIntervalUs = 250000; // 250ms

// ── helpers ─────────────────────────────────────────────────────────

static std::string getRuntimeDir() {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    return runtime ? std::string(runtime) : "/tmp";
}

std::string VibetypeEngine::sessionDir() {
    return getRuntimeDir() + "/vibetype/" + sessionId_;
}

std::string VibetypeEngine::generateUuid() {
    unsigned char bytes[16] = {};
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom) {
        urandom.read(reinterpret_cast<char *>(bytes), 16);
        urandom.close();
    } else {
        // Fallback: not cryptographically random but good enough for session IDs
        std::srand(static_cast<unsigned>(std::time(nullptr)));
        for (int i = 0; i < 16; i++) {
            bytes[i] = static_cast<unsigned char>(std::rand() & 0xFF);
        }
    }
    // Set version 4 (random) and variant bits
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3],
                  bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return buf;
}

// ── ctor / dtor ─────────────────────────────────────────────────────

VibetypeEngine::VibetypeEngine(Instance *instance)
    : instance_(instance) {
    dispatcher_.attach(&instance_->eventLoop());
    reloadConfig();
    connectBackend();
}

VibetypeEngine::~VibetypeEngine() {
    stopCapture();
    disconnectBackend();
    dispatcher_.detach();
}

// ── config ──────────────────────────────────────────────────────────

static constexpr char kConfigFile[] = "conf/vibetype.conf";

void VibetypeEngine::reloadConfig() {
    readAsIni(config_, kConfigFile);
    const auto &keys = config_.triggerKey.value();
    if (!keys.empty()) {
        trigger_key_sym_ = keys[0].sym();
    }
    FCITX_INFO() << "Vibetype config reloaded: triggerKey sym="
                 << static_cast<int>(trigger_key_sym_);
}

const Configuration *VibetypeEngine::getConfig() const {
    return &config_;
}

void VibetypeEngine::setConfig(const RawConfig &raw) {
    config_.load(raw, true);
    safeSaveAsIni(config_, kConfigFile);
}

void VibetypeEngine::setSubConfig(const std::string &path,
                                   const RawConfig &raw) {
    FCITX_UNUSED(path);
    setConfig(raw);
}

// ── backend IPC ─────────────────────────────────────────────────────

void VibetypeEngine::connectBackend() {
    const std::string socketPath = config_.socketPath.value().empty()
        ? getRuntimeDir() + "/vibetype/vibetype.sock"
        : config_.socketPath.value();

    ipc_ = std::make_unique<xtils::IpcClient>(socketPath);

    if (!ipc_->Connect()) {
        FCITX_ERROR() << "Vibetype: cannot connect to backend at " << socketPath;
        backendConnected_ = false;
        return;
    }

    backendConnected_ = true;
    FCITX_INFO() << "Vibetype: connected to backend at " << socketPath;

    // Register notification handler for all backend notifications.
    // Callback runs in the IpcClient read thread; dispatch UI work to event loop.
    subs_.Add(ipc_->OnNotify(
        [this](const std::string &method, const xtils::Json &params) {
            onNotification(method, params);
        }));

    // Send hello
    auto hello = xtils::Json::object();
    hello["client"] = "fcitx5";
    hello["protocol_version"] = 1;
    auto helloResult = safeCall("vibetype.hello", hello, 3000);
    if (!helloResult.ok()) {
        FCITX_WARN() << "Vibetype: hello failed: " << helloResult.error().message;
    }

    checkModel();
}

void VibetypeEngine::disconnectBackend() {
    subs_.DisconnectAll();
    if (ipc_) {
        ipc_->Disconnect();
        ipc_.reset();
    }
    backendConnected_ = false;
}

void VibetypeEngine::checkModel() {
    if (!ipc_ || !backendConnected_)
        return;

    auto result = safeCall("vibetype.modelStatus", xtils::Json::object(), 3000);
    if (!result.ok()) {
        FCITX_WARN() << "Vibetype: modelStatus failed: " << result.error().message;
        return;
    }

    const auto &status = *result;
    std::string state = status.get_string("state").value_or("unknown");
    std::string msg = status.get_string("message").value_or("");

    if (state == "ready") {
        modelReady_ = true;
        dispatcher_.schedule([this]() {
            doStatus("Vibetype model ready");
        });
        if (pendingStart_) {
            pendingStart_ = false;
            dispatcher_.schedule([this]() { startRecording(); });
        }
    } else {
        modelReady_ = false;
        dispatcher_.schedule([this, state, msg]() {
            doStatus("model " + state + ": " + msg);
        });
    }
}

xtils::Result<xtils::Json>
VibetypeEngine::safeCall(const std::string &method,
                          const xtils::Json &params,
                          uint32_t timeout_ms) {
    if (!ipc_ || !backendConnected_) {
        return xtils::Err("backend not connected");
    }
    try {
        return ipc_->Call(method, params, timeout_ms);
    } catch (const std::exception &e) {
        FCITX_ERROR() << "Vibetype: IPC call " << method
                       << " threw: " << e.what();
        return xtils::Err(std::string("IPC exception: ") + e.what());
    } catch (...) {
        FCITX_ERROR() << "Vibetype: IPC call " << method
                       << " threw unknown exception";
        return xtils::Err("IPC unknown exception");
    }
}

void VibetypeEngine::onNotification(const std::string &method,
                                     const xtils::Json &params) {
    if (method == "vibetype.finalResult") {
        std::string text = params.get_string("text").value_or("");
        if (!text.empty()) {
            dispatcher_.schedule([this, text]() {
                stopRecordingAnimation();
                setRecordingState(false);
                clearPanel();
                auto *ic = instance_->mostRecentInputContext();
                if (ic) ic->commitString(text);
                // Clean up session directory
                if (!sessionDir_.empty()) {
                    std::error_code ec;
                    std::filesystem::remove_all(sessionDir_, ec);
                    sessionDir_.clear();
                }
            });
        }
    } else if (method == "vibetype.partialResult") {
        // Partial results: recording animation already shows status.
        // No additional UI update needed.
    } else if (method == "vibetype.error") {
        std::string msg = params.get_string("message").value_or("unknown error");
        FCITX_WARN() << "Vibetype backend error: " << msg;
        dispatcher_.schedule([this, msg]() {
            setRecordingState(false);
            showPanelMessage("error: " + msg);
        });
    } else if (method == "vibetype.modelStatusChanged") {
        std::string state = params.get_string("state").value_or("unknown");
        std::string msg = params.get_string("message").value_or("");
        bool ready = (state == "ready");
        modelReady_ = ready;
        if (ready) {
            dispatcher_.schedule([this]() {
                doStatus("Vibetype model ready");
            });
            if (pendingStart_) {
                pendingStart_ = false;
                dispatcher_.schedule([this]() { startRecording(); });
            }
        } else {
            dispatcher_.schedule([this, state, msg]() {
                showPanelMessage("model " + state + ": " + msg);
            });
        }
    }
}

// ── key event ───────────────────────────────────────────────────────

void VibetypeEngine::keyEvent(const InputMethodEntry &entry,
                               KeyEvent &keyEvent) {
    FCITX_UNUSED(entry);

    if (keyEvent.key().sym() != trigger_key_sym_)
        return;

    keyEvent.filterAndAccept();

    if (keyEvent.isRelease()) {
        stopRecording();
    } else {
        if (!recording_)
            startRecording();
    }
}

// ── recording ───────────────────────────────────────────────────────

void VibetypeEngine::startRecording() {
    if (recording_)
        return;

    if (!backendConnected_) {
        connectBackend();
        if (!backendConnected_) {
            doStatus("Vibetype: cannot connect to backend");
            return;
        }
    }

    if (!modelReady_) {
        pendingStart_ = true;
        doStatus("Vibetype: model not ready, waiting...");
        checkModel();
        return;
    }

    recording_ = true;
    startRecordingAnimation();
    beginSession();
    startCapture();
}

void VibetypeEngine::stopRecording() {
    if (!recording_)
        return;

    recording_ = false;
    stopRecordingAnimation();

    stopCapture();

    // Check minimum recording duration
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - recordStartTime_).count();

    if (elapsed < static_cast<decltype(elapsed)>(kMinRecordUs / 1000)) {
        doStatus("cancelled: recording too short (" +
                 std::to_string(elapsed) + "ms)");
        xtils::Json params = xtils::Json::object();
        params["session_id"] = sessionId_;
        safeCall("vibetype.cancelSession", params, 3000);
        // Clean up
        if (!sessionDir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(sessionDir_, ec);
            sessionDir_.clear();
        }
        dispatcher_.schedule([this]() { clearPanel(); });
        return;
    }

    doStatus("⏳ Processing...");

    xtils::Json params = xtils::Json::object();
    params["session_id"] = sessionId_;
    params["segment_count"] = segmentCount_;
    auto result = safeCall("vibetype.finishSession", params, kSessionTimeoutMs);
    if (!result.ok()) {
        doStatus("error: finish failed: " + result.error().message);
        dispatcher_.schedule([this]() { clearPanel(); });
    }
    // finalResult notification will trigger doCommit and panel clear
}

void VibetypeEngine::beginSession() {
    sessionId_ = generateUuid();
    sessionDir_ = sessionDir();
    segmentIndex_ = 0;
    segmentCount_ = 0;

    // Create session directory
    std::error_code ec;
    std::filesystem::create_directories(sessionDir_, ec);
    if (ec) {
        FCITX_ERROR() << "Vibetype: cannot create session dir " << sessionDir_;
        return;
    }

    xtils::Json params = xtils::Json::object();
    params["session_id"] = sessionId_;
    auto af = xtils::Json::object();
    af["sample_rate"] = 16000;
    af["channels"] = 1;
    af["sample_format"] = "pcm_s16le";
    af["container"] = "wav";
    params["audio_format"] = af;
    params["frontend"] = "fcitx5";

    auto result = safeCall("vibetype.startSession", params, kSessionTimeoutMs);
    if (!result.ok()) {
        FCITX_ERROR() << "Vibetype: startSession failed: "
                       << result.error().message;
        doStatus("error: session start failed");
    }
}

// ── capture ─────────────────────────────────────────────────────────

void VibetypeEngine::startCapture() {
    if (capturing_)
        return;

    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        FCITX_ERROR() << "Vibetype: pipe failed";
        return;
    }

    const std::string device = config_.audioDevice.value();

    pid_t pid = ::fork();
    if (pid == -1) {
        FCITX_ERROR() << "Vibetype: fork failed";
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        /* ── child: exec arecord ── */
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        /* close all other fds */
        for (int fd = 3; fd < 256; ++fd)
            ::close(fd);

        ::execlp("arecord", "arecord", "-q", "-D", device.c_str(),
                 "-f", "S16_LE", "-r", "16000", "-c", "1", "-t", "raw",
                 nullptr);
        // If exec fails, exit child
        ::_exit(127);
    }

    /* ── parent ── */
    ::close(pipefd[1]);
    arecordPid_ = pid;
    capturePipe_ = pipefd[0];
    recordStartTime_ = std::chrono::steady_clock::now();
    capturing_ = true;
    captureThread_ = std::make_unique<std::thread>([this]() { captureLoop(); });
}

void VibetypeEngine::stopCapture() {
    if (!capturing_)
        return;
    capturing_ = false;

    // Terminate arecord
    if (arecordPid_ > 0) {
        ::kill(arecordPid_, SIGTERM);
        int status;
        ::waitpid(arecordPid_, &status, 0);
        arecordPid_ = -1;
    }

    // Close pipe
    if (capturePipe_ != -1) {
        ::close(capturePipe_);
        capturePipe_ = -1;
    }

    // Join capture thread
    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }
}

void VibetypeEngine::captureLoop() {
    constexpr int kSampleRate = 16000;
    constexpr int kBytesPerSecond = kSampleRate * 2;  // 16-bit mono
    const int segmentBytes = kBytesPerSecond * config_.segmentSeconds.value();
    constexpr int kChunkBytes = 3200;  // 100ms of audio

    std::vector<uint8_t> pcm;
    pcm.reserve(static_cast<size_t>(segmentBytes) + kChunkBytes);
    auto segmentStart = std::chrono::steady_clock::now();

    while (capturing_) {
        std::vector<uint8_t> chunk(static_cast<size_t>(kChunkBytes));
        ssize_t n = ::read(capturePipe_, chunk.data(),
                           static_cast<size_t>(kChunkBytes));
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;  // retry on signal interrupt
            break;
        }
        chunk.resize(static_cast<size_t>(n));
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());

        // Emit segments at boundary
        while (pcm.size() >= static_cast<size_t>(segmentBytes)) {
            std::vector<uint8_t> segment(
                pcm.begin(),
                pcm.begin() + static_cast<size_t>(segmentBytes));
            pcm.erase(
                pcm.begin(),
                pcm.begin() + static_cast<size_t>(segmentBytes));

            auto now = std::chrono::steady_clock::now();
            int durationMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - segmentStart).count());
            int idx = segmentIndex_++;

            // Write WAV and submit
            std::string wavPath = sessionDir_ + "/segment-" +
                (idx < 10 ? "00" : idx < 100 ? "0" : "") +
                std::to_string(idx) + ".wav";
            if (writeWav(wavPath, segment)) {
                xtils::Json sp = xtils::Json::object();
                sp["session_id"] = sessionId_;
                sp["segment_index"] = idx;
                sp["wav_path"] = wavPath;
                sp["duration_ms"] = durationMs;
                auto r = safeCall("vibetype.transcribeSegment",
                                  sp, kSegmentTimeoutMs);
                if (r.ok()) {
                    segmentCount_ = std::max(segmentCount_, idx + 1);
                }
            }
            segmentStart = now;
        }
    }

    // Flush remaining PCM
    if (!pcm.empty()) {
        auto now = std::chrono::steady_clock::now();
        int durationMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - segmentStart).count());
        int idx = segmentIndex_++;

        std::string wavPath = sessionDir_ + "/segment-" +
            (idx < 10 ? "00" : idx < 100 ? "0" : "") +
            std::to_string(idx) + ".wav";
        if (writeWav(wavPath, pcm)) {
            xtils::Json sp = xtils::Json::object();
            sp["session_id"] = sessionId_;
            sp["segment_index"] = idx;
            sp["wav_path"] = wavPath;
            sp["duration_ms"] = durationMs;
            auto r = safeCall("vibetype.transcribeSegment", sp,
                              kSegmentTimeoutMs);
            if (r.ok()) {
                segmentCount_ = std::max(segmentCount_, idx + 1);
            }
        }
    }
}

bool VibetypeEngine::writeWav(const std::string &path,
                               const std::vector<uint8_t> &pcm) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        FCITX_ERROR() << "Vibetype: cannot write WAV " << path;
        return false;
    }

    auto write16 = [&](uint16_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
    };
    auto write32 = [&](uint32_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>((v >> 8) & 0xFF));
        out.put(static_cast<char>((v >> 16) & 0xFF));
        out.put(static_cast<char>((v >> 24) & 0xFF));
    };

    uint32_t dataSize = static_cast<uint32_t>(pcm.size());
    uint32_t fileSize = 36 + dataSize;

    // RIFF header
    out.write("RIFF", 4);
    write32(fileSize);
    out.write("WAVE", 4);

    // fmt chunk
    out.write("fmt ", 4);
    write32(16);        // chunk size
    write16(1);         // PCM
    write16(1);         // mono
    write32(16000);     // sample rate
    write32(32000);     // byte rate
    write16(2);         // block align
    write16(16);        // bits per sample

    // data chunk
    out.write("data", 4);
    write32(dataSize);
    out.write(reinterpret_cast<const char *>(pcm.data()),
              static_cast<std::streamsize>(dataSize));

    if (!out) {
        FCITX_ERROR() << "Vibetype: WAV write failed for " << path;
        return false;
    }
    return true;
}

// ── panel UI ───────────────────────────────────────────────────────

void VibetypeEngine::setRecordingState(bool on) {
    if (recording_ == on)
        return;
    recording_ = on;
    if (!on) {
        stopRecordingAnimation();
        dispatcher_.schedule([this]() { clearPanel(); });
    }
}

void VibetypeEngine::showPanelMessage(const std::string &message) {
    /* Must be called from event loop thread (dispatched).
     * Uses AuxUp (top bar) like VocoType. */
    auto *ic = instance_->mostRecentInputContext();
    if (!ic)
        return;
    auto &panel = ic->inputPanel();
    fcitx::Text display(message);
    panel.setAuxUp(display);
    panel.setAuxDown(fcitx::Text());
    panel.setClientPreedit(fcitx::Text());
    panel.setPreedit(fcitx::Text());
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void VibetypeEngine::clearPanel() {
    auto *ic = instance_->mostRecentInputContext();
    if (!ic)
        return;
    auto &panel = ic->inputPanel();
    panel.setAuxUp(fcitx::Text());
    panel.setAuxDown(fcitx::Text());
    panel.setClientPreedit(fcitx::Text());
    panel.setPreedit(fcitx::Text());
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

// ── recording animation (event-loop timer, like VocoType) ──────────

void VibetypeEngine::startRecordingAnimation() {
    stopRecordingAnimation();
    recording_animation_frame_index_ = 0;
    showAnimationFrame();
    scheduleNextAnimationFrame();
}

void VibetypeEngine::stopRecordingAnimation() {
    recording_animation_timer_.reset();
    recording_animation_frame_index_ = 0;
}

void VibetypeEngine::showAnimationFrame() {
    const auto &frame =
        kRecordingFrames[recording_animation_frame_index_ %
                          kRecordingFrames.size()];
    showPanelMessage(frame);
    recording_animation_frame_index_ =
        (recording_animation_frame_index_ + 1) % kRecordingFrames.size();
}

void VibetypeEngine::scheduleNextAnimationFrame() {
    recording_animation_timer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        now(CLOCK_MONOTONIC) + kAnimationIntervalUs,
        0,
        [this](EventSourceTime *, uint64_t) {
            recording_animation_timer_.reset();
            if (!recording_)
                return false;
            showAnimationFrame();
            scheduleNextAnimationFrame();
            return false;
        });
    recording_animation_timer_->setOneShot();
}

// ── fcitx thread-safe UI updates ───────────────────────────────────

void VibetypeEngine::doCommit(const std::string &text) {
    if (text.empty())
        return;
    dispatcher_.schedule([this, text]() {
        clearPanel();
        auto *ic = instance_->mostRecentInputContext();
        if (!ic)
            return;
        ic->commitString(text);
    });
}

void VibetypeEngine::doStatus(const std::string &text) {
    dispatcher_.schedule([this, text]() {
        showPanelMessage(text);
    });
}

// ── factory ─────────────────────────────────────────────────────────

AddonInstance *VibetypeFactory::create(AddonManager *manager) {
    return new VibetypeEngine(manager->instance());
}

FCITX_ADDON_FACTORY(VibetypeFactory);

} // namespace fcitx
