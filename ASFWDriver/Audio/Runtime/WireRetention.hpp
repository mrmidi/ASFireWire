// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// WireRetention.hpp - whether the device's wire outlives CoreAudio IO.
//
// AppleFWAudio keeps a device's isochronous streams running from device start to
// device stop; CoreAudio's engine start/stop only attaches and detaches the
// engine (AppleFWAudioIsocEngine::performAudioEngineStop @0x7ba calls only
// AppleFWAudioDevice::EngineStopped; StartAllStreams/StopAllStreams run from
// StartAudioDevice/StopAudioDevice and the format change). A slow device never
// fails an engine start: performAudioEngineStart @0x33a2 waits up to 10 s for
// IsDeviceReady and then continues.
//
// ASFW keeps the wire on StopIO for profiles that opt in, so CoreAudio's
// StopIO/StartIO churn costs no bus traffic, and a start whose first timestamp
// is late keeps the warming wire instead of tearing it down (which restarts the
// device's own warm-up; BeBoB devices send only NO-DATA for up to ~4 s,
// Linux bebob_stream.c:10,661). Only a sample-rate change, an IO restart, a
// transport fault or driver stop release a kept wire.
//
// Pure decision logic; the DriverKit glue lives in ASFWAudioDevice.cpp.

#pragma once

#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class WireState : uint8_t {
    kDown,      ///< No wire: the next StartIO brings it up.
    kLive,      ///< Wire up and CoreAudio IO running on it.
    kRetained,  ///< Wire up, CoreAudio IO stopped (or its start timed out).
};

enum class WireReleaseReason : uint8_t {
    kNotRetainable,   ///< Profile does not opt in.
    kRateChange,
    kIoRestart,
    kTransportFault,
    kDriverStop,
    kRateMismatch,    ///< StartIO at a rate the kept wire does not carry.
    kTimeoutLimit,    ///< Too many consecutive first-timestamp timeouts.
    kHalStartFailed,  ///< The HAL refused the start on a joined wire.
};

[[nodiscard]] constexpr const char* WireStateName(WireState s) noexcept {
    switch (s) {
        case WireState::kDown: return "down";
        case WireState::kLive: return "live";
        case WireState::kRetained: return "retained";
    }
    return "?";
}

[[nodiscard]] constexpr const char* WireReleaseReasonName(WireReleaseReason r) noexcept {
    switch (r) {
        case WireReleaseReason::kNotRetainable: return "not-retainable";
        case WireReleaseReason::kRateChange: return "rate-change";
        case WireReleaseReason::kIoRestart: return "io-restart";
        case WireReleaseReason::kTransportFault: return "transport-fault";
        case WireReleaseReason::kDriverStop: return "driver-stop";
        case WireReleaseReason::kRateMismatch: return "rate-mismatch";
        case WireReleaseReason::kTimeoutLimit: return "timeout-limit";
        case WireReleaseReason::kHalStartFailed: return "hal-start-failed";
    }
    return "?";
}

/// What StartIO does with the wire.
enum class WireStartPlan : uint8_t {
    kFreshStart,          ///< No wire: full bring-up.
    kRejoin,              ///< Join the kept wire.
    kReleaseThenFresh,    ///< Kept wire is unusable: release, then full bring-up.
};

/// What a stop (or a timed-out start) does with the wire.
enum class WireStopPlan : uint8_t {
    kRelease,
    kRetain,
};

class WireRetention final {
public:
    /// A start that times out this many times in a row on one kept wire stops
    /// keeping it: the next start is a full bring-up (AppleFWAudio resets the
    /// streams after each 10 s readiness timeout, performFormatChange @0x151c).
    static constexpr uint32_t kMaxConsecutiveStartTimeouts = 3;

    [[nodiscard]] WireState State() const noexcept { return state_; }
    [[nodiscard]] uint32_t RateHz() const noexcept { return rateHz_; }
    [[nodiscard]] uint32_t ConsecutiveStartTimeouts() const noexcept { return startTimeouts_; }
    [[nodiscard]] bool IoRestartPending() const noexcept { return ioRestartPending_; }

    /// StartIO at `rateHz`. `transportHealthy` is false after a transport fault
    /// or an unresolved stop.
    [[nodiscard]] WireStartPlan PlanStart(uint32_t rateHz, bool transportHealthy,
                                          WireReleaseReason* releaseReason) const noexcept {
        if (state_ != WireState::kRetained) return WireStartPlan::kFreshStart;
        WireReleaseReason reason{};
        if (ioRestartPending_) {
            reason = WireReleaseReason::kIoRestart;
        } else if (!transportHealthy) {
            reason = WireReleaseReason::kTransportFault;
        } else if (rateHz != rateHz_) {
            reason = WireReleaseReason::kRateMismatch;
        } else {
            return WireStartPlan::kRejoin;
        }
        if (releaseReason) *releaseReason = reason;
        return WireStartPlan::kReleaseThenFresh;
    }

    /// The wire carries CoreAudio IO now (fresh start or rejoin succeeded).
    /// A pending IO restart survives it: one requested while the start was
    /// finishing must still release the wire at the window's StopIO. Only a
    /// release clears it.
    void OnStarted(uint32_t rateHz) noexcept {
        state_ = WireState::kLive;
        rateHz_ = rateHz;
        startTimeouts_ = 0;
    }

    /// A start brought the wire up (or joined it) but the first timestamp did
    /// not arrive in time. Keeping the wire lets the HAL's retry join the
    /// device's warm-up instead of restarting it.
    [[nodiscard]] WireStopPlan PlanStartTimeout(bool retainable, bool transportHealthy,
                                                WireReleaseReason* releaseReason) const noexcept {
        WireReleaseReason reason{};
        if (!retainable) {
            reason = WireReleaseReason::kNotRetainable;
        } else if (ioRestartPending_) {
            reason = WireReleaseReason::kIoRestart;
        } else if (!transportHealthy) {
            reason = WireReleaseReason::kTransportFault;
        } else if (startTimeouts_ + 1 >= kMaxConsecutiveStartTimeouts) {
            reason = WireReleaseReason::kTimeoutLimit;
        } else {
            return WireStopPlan::kRetain;
        }
        if (releaseReason) *releaseReason = reason;
        return WireStopPlan::kRelease;
    }

    void OnStartTimedOutRetained(uint32_t rateHz) noexcept {
        state_ = WireState::kRetained;
        rateHz_ = rateHz;
        ++startTimeouts_;
    }

    /// StopIO on a live wire.
    [[nodiscard]] WireStopPlan PlanStop(bool retainable, bool transportHealthy,
                                        WireReleaseReason* releaseReason) const noexcept {
        WireReleaseReason reason{};
        if (!retainable) {
            reason = WireReleaseReason::kNotRetainable;
        } else if (ioRestartPending_) {
            // The IO restart exists to rebuild the audio-owned TX queue and
            // prefill (ASFWAudioDevice kConfigChangeActionIoRestart); keeping
            // the old queue is the failure it fixes.
            reason = WireReleaseReason::kIoRestart;
        } else if (!transportHealthy) {
            reason = WireReleaseReason::kTransportFault;
        } else {
            return WireStopPlan::kRetain;
        }
        if (releaseReason) *releaseReason = reason;
        return WireStopPlan::kRelease;
    }

    void OnRetained() noexcept { state_ = WireState::kRetained; }

    /// The wire was released (or never came up).
    void OnReleased() noexcept {
        state_ = WireState::kDown;
        rateHz_ = 0;
        startTimeouts_ = 0;
        ioRestartPending_ = false;
    }

    /// The transport asked for an IO restart. Whatever StopIO/StartIO follows
    /// must rebuild the wire, not keep it.
    void OnIoRestartRequested() noexcept { ioRestartPending_ = true; }

    /// True when an event that invalidates a kept wire must release it now
    /// (nothing else will: CoreAudio IO is not running to stop it).
    [[nodiscard]] bool MustReleaseOnInvalidation() const noexcept {
        return state_ == WireState::kRetained;
    }

private:
    WireState state_{WireState::kDown};
    uint32_t rateHz_{0};
    uint32_t startTimeouts_{0};
    bool ioRestartPending_{false};
};

} // namespace ASFW::Audio::Runtime
