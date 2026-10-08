// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceFamilyAdapter.hpp - DICE/TCAT behind the audio device host.
//
// documentation/AUDIO_DEVICE_HOST.md §6 E5. What DiceAudioBackend did that is
// DICE's own:
//   - Describe: the device's registers describe its streams (EnsureNubForGuid's
//     body). Every refusal it made is kept, now named, so the host's
//     [AudioHost] line says why a device was not published.
//   - JudgeRuntimeFault: a runtime fault whose device still reports a locked,
//     healthy clock self-heals in the RX epoch reset; anything else restarts.
//   - Device events: the notification mailbox's bits become neutral events. A
//     config change restarts the run; a lock or ext-status change asks the host
//     for its clock probe (§4.4).
//
// The protocol-detail lines (geometry per stream, applied runtime geometry,
// channel labels, named-from-geometry) keep their text: hardware notes are read
// by them (§6.1).

#pragma once

#include "FamilyAdapter.hpp"

#include <atomic>

namespace ASFW::Audio::DICE {
class DiceNotificationRouter;
}

namespace ASFW::Audio::Host {

class DiceFamilyAdapter final : public FamilyAdapter {
public:
    explicit DiceFamilyAdapter(DICE::DiceNotificationRouter& notifications) noexcept;
    ~DiceFamilyAdapter() noexcept override;

    DiceFamilyAdapter(const DiceFamilyAdapter&) = delete;
    DiceFamilyAdapter& operator=(const DiceFamilyAdapter&) = delete;

    /// Timeout for the runtime-fault health read (DiceAudioBackend's
    /// kHealthBridgeTimeoutMs).
    static constexpr uint32_t kHealthReadTimeoutMs = 1000;

    // DescribeRefusal reasons, one per refusal EnsureNubForGuid made.
    static constexpr const char* kNoProfile = "no-dice-profile";
    static constexpr const char* kNoProtocol = "no-protocol";
    static constexpr const char* kGeometryLoadFailed = "geometry-load-failed";
    static constexpr const char* kCapsUnavailable = "runtime-caps-unavailable";
    static constexpr const char* kGeometryUnusable = "geometry-unusable";
    static constexpr const char* kNoStreamableRate = "no-streamable-rate";
    static constexpr const char* kNoEnabledFormation = "no-enabled-formation";
    static constexpr const char* kFormationNotSelectable = "formation-not-selectable";
    static constexpr const char* kTooManyPlaybackStreams = "too-many-playback-streams";

    [[nodiscard]] const char* Name() const noexcept override { return "DICE"; }

    void Describe(const DescribeInput& in, DescribeDone done) override;

    [[nodiscard]] bool ActsOn(DuplexRestartReason reason) const noexcept override {
        // DICE acts on every fault, cycle inconsistent included
        // (DiceAudioBackend::HandleCycleInconsistent).
        (void)reason;
        return true;
    }

    [[nodiscard]] FaultVerdict JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                FaultContext& context) override;

    /// Subscribes to the notification mailbox while a sink is installed; null
    /// unsubscribes. After it returns with null, no notification is running or
    /// will start (DiceNotificationRouter::ClearObserver).
    void SetEventSink(DeviceEventSink* sink) noexcept override;

    /// The mailbox's bits for `guid`, as neutral events. Public for tests.
    void OnNotification(uint64_t guid, uint32_t bits) noexcept;

private:
    static void NotificationThunk(void* context, uint64_t guid, uint32_t bits) noexcept;

    DICE::DiceNotificationRouter& notifications_;
    std::atomic<DeviceEventSink*> sink_{nullptr};
};

} // namespace ASFW::Audio::Host
