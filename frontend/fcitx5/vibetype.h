/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025-2027 Lingzo Labs
 */

#ifndef VIBETYPE_FCITX5_ENGINE_H_
#define VIBETYPE_FCITX5_ENGINE_H_

#include <atomic>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx-utils/eventdispatcher.h>
#include <memory>
#include <string>
#include <thread>

namespace fcitx {

class Instance;

// ── Config ──────────────────────────────────────────────────────────

FCITX_CONFIGURATION(VibetypeConfig,
                     KeyListOption triggerKey{
                         this, "TriggerKey", "Trigger Key", {Key("F12")},
                         KeyListConstrain()};
                     Option<int, IntConstrain> segmentSeconds{
                         this, "SegmentSeconds", "Segment Seconds", 20,
                         IntConstrain(5, 60)};
                     Option<std::string> socketPath{
                         this, "SocketPath", "Backend Socket Path", ""};
                     Option<std::string> audioDevice{
                         this, "AudioDevice", "ALSA Audio Device",
                         "default"};);

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
    /* subprocess management */
    void toggleRecording();
    void startHelper();
    void stopHelper();
    void sendLine(const std::string &line);
    void readerLoop();

    /* recording */
    static std::string
    defaultSocketPath();

    /* fcitx thread-safe commit/status */
    void doCommit(const std::string &text);
    void doStatus(const std::string &text);
    void setRecordingState(bool on);

    Instance *instance_;
    EventDispatcher dispatcher_;
    VibetypeConfig config_;

    /* subprocess state */
    pid_t childPid_ = -1;
    int pipeStdin_ = -1;  /* write end → child stdin */
    int pipeStdout_ = -1; /* read end  ← child stdout */
    std::unique_ptr<std::thread> readerThread_;
    std::atomic<bool> running_{false};

    /* recording / model state */
    bool recording_ = false;
    bool modelReady_ = false;
    bool pendingStart_ = false;
};

// ── Factory ─────────────────────────────────────────────────────────

class VibetypeFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override;
};

} // namespace fcitx

#endif /* VIBETYPE_FCITX5_ENGINE_H_ */
