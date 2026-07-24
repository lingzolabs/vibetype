/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025-2027 Lingzo Labs
 */

#ifndef VIBETYPE_FCITX5_ENGINE_H_
#define VIBETYPE_FCITX5_ENGINE_H_

#include <atomic>
#include <array>
#include <chrono>
#include <fcitx-config/option.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/key.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "xtils/net/ipc_channel.h"
#include "xtils/utils/json.h"
#include "xtils/utils/result.h"
#include "xtils/utils/signal.h"

namespace fcitx {

class Instance;

// ── Config ──────────────────────────────────────────────────────────

FCITX_CONFIGURATION(VibetypeConfig,
                     KeyListOption triggerKey{
                         this, "TriggerKey", "Trigger Key", {Key("F12")},
                         KeyListConstrain({KeyConstrainFlag::AllowModifierLess,
                                          KeyConstrainFlag::AllowModifierOnly})};
                     Option<int, IntConstrain> segmentSeconds{
                         this, "SegmentSeconds", "Segment Seconds", 20,
                         IntConstrain(5, 60)};
                     Option<std::string> socketPath{
                         this, "SocketPath", "Backend Socket Path", ""};
                     Option<std::string> audioDevice{
                         this, "AudioDevice", "ALSA Audio Device",
                         "default"};
                     // ── Text-processing (synced from text-processing.json) ──
                     Option<bool> enableBuiltinCorrections{
                         this, "EnableBuiltinCorrections",
                         "Enable built-in computer-term corrections", true};
                     Option<bool> enableQwenPolish{
                         this, "EnableQwenPolish",
                         "Enable Qwen LLM polish", false};
                     // Multi-line: each line is "from=to"
                     Option<std::string> customCorrections{
                         this, "CustomCorrections",
                         "Custom corrections (one per line: wrong=correct)", ""};
                    );

// ── Engine ──────────────────────────────────────────────────────────

class VibetypeEngine : public InputMethodEngineV2 {
public:
    VibetypeEngine(Instance *instance);
    ~VibetypeEngine() override;

    void keyEvent(const InputMethodEntry &entry,
                  KeyEvent &keyEvent) override;

    void reloadConfig() override;
    const Configuration *getConfig() const override;
    void setConfig(const RawConfig &config) override;

    void setSubConfig(const std::string &path,
                      const RawConfig &config) override;

private:
    /* shared text-processing config */
    void syncTextProcFromFile();
    void flushTextProcToFile();

    /* backend IPC */
    void connectBackend();
    void disconnectBackend();
    void checkModel();
    void onNotification(const std::string &method,
                        const xtils::Json &params);
    xtils::Result<xtils::Json>
    safeCall(const std::string &method,
             const xtils::Json &params = xtils::Json::object(),
             uint32_t timeout_ms = 5000);

    /* recording */
    void startRecording();
    void stopRecording();
    bool beginSession();
    void startCapture();
    void stopCapture();
    void captureLoop();
    std::string generateUuid();

    /* WAV helpers */
    bool writeWav(const std::string &path,
                  const std::vector<uint8_t> &pcm);
    std::string sessionDir();

    /* fcitx thread-safe commit/status */
    void doCommit(const std::string &text);
    void doStatus(const std::string &text);
    void setRecordingState(bool on);

    /* panel UI (must be called from event loop thread) */
    void showPanelMessage(const std::string &message);
    void clearPanel();

    /* recording animation */
    void startRecordingAnimation();
    void stopRecordingAnimation();
    void showAnimationFrame();
    void scheduleNextAnimationFrame();

    Instance *instance_;
    EventDispatcher dispatcher_;
    VibetypeConfig config_;

    /* trigger key */
    KeySym trigger_key_sym_ = FcitxKey_F12;

    /* recording animation state */
    std::unique_ptr<EventSourceTime> recording_animation_timer_;
    size_t recording_animation_frame_index_ = 0;

    /* ── IPC ── */
    std::unique_ptr<xtils::IpcClient> ipc_;
    xtils::ScopedSubscriptions subs_;
    std::atomic<bool> backendConnected_{false};
    std::atomic<bool> modelReady_{false};
    bool pendingStart_ = false;

    /* ── Recording ── */
    std::atomic<bool> recording_{false};
    std::atomic<bool> capturing_{false};
    pid_t arecordPid_ = -1;
    int capturePipe_ = -1;
    std::unique_ptr<std::thread> captureThread_;
    std::string sessionId_;
    std::string sessionDir_;
    int segmentIndex_ = 0;
    int segmentCount_ = 0;
    std::chrono::steady_clock::time_point recordStartTime_;

    static constexpr uint32_t kSegmentTimeoutMs = 120000;
    static constexpr uint32_t kSessionTimeoutMs = 5000;
    static constexpr uint64_t kMinRecordUs = 1000000;  // 1 second
};

// ── Factory ─────────────────────────────────────────────────────────

class VibetypeFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override;
};

} // namespace fcitx

#endif /* VIBETYPE_FCITX5_ENGINE_H_ */
