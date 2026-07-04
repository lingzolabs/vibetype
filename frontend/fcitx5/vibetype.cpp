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
#include <csignal>
#include <cstdlib>
#include <cstring>
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

static std::vector<std::string>
splitLines(const std::string &buf) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < buf.size()) {
        auto end = buf.find('\n', start);
        if (end == std::string::npos)
            break;
        out.push_back(buf.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string VibetypeEngine::defaultSocketPath() {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    std::string dir = runtime ? runtime : "/tmp";
    return dir + "/vibetype/vibetype.sock";
}

// ── ctor / dtor ─────────────────────────────────────────────────────

VibetypeEngine::VibetypeEngine(Instance *instance)
    : instance_(instance) {
    dispatcher_.attach(&instance_->eventLoop());
    reloadConfig();
    startHelper();
}

VibetypeEngine::~VibetypeEngine() {
    stopHelper();
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
    FCITX_INFO() << "Vibetype config loaded: triggerKey sym="
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

// ── key event ───────────────────────────────────────────────────────

void VibetypeEngine::keyEvent(const InputMethodEntry &entry,
                               KeyEvent &keyEvent) {
    FCITX_UNUSED(entry);

    /* Compare key sym directly (supports modifier keys like Right Ctrl) */
    if (keyEvent.key().sym() != trigger_key_sym_)
        return;

    keyEvent.filterAndAccept();

    if (keyEvent.isRelease()) {
        /* Release → stop recording if active */
        stopRecording();
    } else {
        /* Press → start recording (ignore auto-repeat) */
        if (!recording_)
            startRecording();
    }
}

// ── subprocess management ───────────────────────────────────────────

void VibetypeEngine::startHelper() {
    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};

    if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0) {
        FCITX_ERROR() << "Vibetype: failed to create pipes";
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        FCITX_ERROR() << "Vibetype: fork failed";
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return;
    }

    if (pid == 0) {
        /* ── child ── */
        close(stdinPipe[1]);  /* close write end of stdin pipe */
        close(stdoutPipe[0]); /* close read end of stdout pipe */

        /* redirect stdin */
        dup2(stdinPipe[0], STDIN_FILENO);
        close(stdinPipe[0]);

        /* redirect stdout */
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdoutPipe[1]);

        /* close all other fds (keep 0,1,2) */
        for (int fd = 3; fd < 256; ++fd)
            close(fd);

        /* build helper path: look next to the installed addon library,
         * then in PATH as fallback */
        std::string helper = "vibetype-fcitx5-helper";
        std::vector<const char *> args = {helper.c_str(), nullptr};
        execvp(args[0], const_cast<char *const *>(args.data()));

        /* if exec fails, fall back to python script directly */
        static const char *python = "/usr/bin/python3";
        std::vector<const char *> pyArgs = {
            python, "-m", "vibetype_fcitx5_helper", nullptr};
        execvp(python, const_cast<char *const *>(pyArgs.data()));

        _exit(127);
    }

    /* ── parent ── */
    close(stdinPipe[0]);  /* close read end */
    close(stdoutPipe[1]); /* close write end */

    childPid_ = pid;
    pipeStdin_ = stdinPipe[1];
    pipeStdout_ = stdoutPipe[0];
    running_ = true;

    readerThread_ = std::make_unique<std::thread>(
        [this] { readerLoop(); });

    FCITX_INFO() << "Vibetype: helper started, pid=" << pid;
}

void VibetypeEngine::stopHelper() {
    running_ = false;

    /* tell helper to quit */
    if (pipeStdin_ != -1) {
        sendLine("quit");
        close(pipeStdin_);
        pipeStdin_ = -1;
    }

    if (readerThread_ && readerThread_->joinable()) {
        readerThread_->join();
        readerThread_.reset();
    }

    if (pipeStdout_ != -1) {
        close(pipeStdout_);
        pipeStdout_ = -1;
    }

    if (childPid_ > 0) {
        int status;
        kill(childPid_, SIGTERM);
        waitpid(childPid_, &status, WNOHANG);
        childPid_ = -1;
    }
}

void VibetypeEngine::sendLine(const std::string &line) {
    if (pipeStdin_ == -1)
        return;
    std::string msg = line + "\n";
    const char *data = msg.data();
    size_t remaining = msg.size();
    while (remaining > 0) {
        ssize_t n = write(pipeStdin_, data, remaining);
        if (n <= 0)
            break;
        data += n;
        remaining -= static_cast<size_t>(n);
    }
}

void VibetypeEngine::readerLoop() {
    char buf[4096];
    std::string leftover;

    while (running_) {
        ssize_t n = read(pipeStdout_, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;

        buf[n] = '\0';
        leftover += buf;

        auto lines = splitLines(leftover);
        if (!lines.empty()) {
            /* keep incomplete last line in leftover */
            size_t lastNewline = leftover.rfind('\n');
            if (lastNewline != std::string::npos) {
                leftover = leftover.substr(lastNewline + 1);
            }

            for (const auto &line : lines) {
                if (line.empty())
                    continue;
                if (line.rfind("status:", 0) == 0) {
                    doStatus(line.substr(7));
                } else if (line.rfind("commit:", 0) == 0) {
                    doCommit(line.substr(7));
                } else if (line.rfind("error:", 0) == 0) {
                    FCITX_WARN() << "Vibetype helper error: "
                                 << line.substr(6);
                    doStatus("error: " + line.substr(6));
                } else if (line == "ready") {
                    modelReady_ = true;
                    doStatus("Vibetype model ready");
                    if (pendingStart_) {
                        pendingStart_ = false;
                        startRecording();
                    }
                } else if (line == "recording" || line == "recording-on") {
                    setRecordingState(true);
                } else if (line == "recording-off" || line == "stopped") {
                    setRecordingState(false);
                }
            }
        }
    }
}

// ── recording ───────────────────────────────────────────────────────

void VibetypeEngine::startRecording() {
    if (recording_)
        return;

    if (!modelReady_) {
        pendingStart_ = true;
        doStatus("Vibetype: model not ready, waiting...");
        sendLine("status");
        return;
    }

    recording_ = true;
    startRecordingAnimation();

    /* pass any configured overrides to the helper */
    std::ostringstream cfg;
    cfg << "config.socket_path="
        << (config_.socketPath.value().empty()
                ? defaultSocketPath()
                : config_.socketPath.value())
        << "\n"
        << "config.segment_seconds="
        << config_.segmentSeconds.value() << "\n"
        << "config.audio_device="
        << config_.audioDevice.value() << "\n";
    sendLine(cfg.str());
    sendLine("record");
}

void VibetypeEngine::stopRecording() {
    if (!recording_)
        return;
    stopRecordingAnimation();
    sendLine("stop");
}

void VibetypeEngine::setRecordingState(bool on) {
    if (recording_ == on)
        return;
    recording_ = on;
    if (!on) {
        stopRecordingAnimation();
    }
    /* recording_ is set eagerly in startRecording(), so this is normally
     * a no-op on start. On stop it is set by the helper's recording-off
     * message or by doCommit(). */
}

// ── panel UI ───────────────────────────────────────────────────────

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
    /* Called from event-loop timer → already on main thread */
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
    stopRecordingAnimation();
    setRecordingState(false);
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
