// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OutputMutePolicy.hpp - Mute for a device that has no mute register.
//
// MOTU v2 exposes an output level but no mute (snd-firewire-ctl-services reports the
// register DSP output volume with mute_avail: false), while macOS drives the mute key only
// from a 'mute' control. So mute here is "write the minimum level", and unmute is "write
// the level the control still holds" -- the volume control's own value is the level to come
// back to, which is why nothing separate is saved and nothing can drift out of step with
// the slider or the knob.
//
// This is host-side behaviour standing in for a hardware parameter, which
// documentation/AUDIO_BACKENDS_CONTROLS.md deliberately avoids ("backend = control").
// It is kept here, as pure decisions with no ADK or DriverKit in sight, so it stays small,
// testable, and easy to delete if a real mute ever turns up.

#pragma once

namespace ASFW::Audio::DriverKit {

/// Half a step of the MOTU's 0.5 dB level: close enough to call two levels equal.
inline constexpr float kLevelEpsilonDb = 0.25f;

/// The level to send the device for a mute state. Unmuting returns to the level the volume
/// control holds, which mute never changed.
[[nodiscard]] constexpr float LevelForMute(bool muted, float controlDb, float minDb) noexcept {
    return muted ? minDb : controlDb;
}

/// What to do when the device reports the level of its own knob.
struct DeviceReportDecision final {
    /// The device is audible again although we believe it is muted: someone turned the
    /// knob, so the mute control no longer reflects reality.
    bool clearMute{false};
    /// Move the volume control to the reported level.
    bool applyToControl{false};

    friend constexpr bool operator==(const DeviceReportDecision&,
                                     const DeviceReportDecision&) = default;
};

[[nodiscard]] constexpr DeviceReportDecision DecideDeviceReport(bool muted,
                                                                float reportedDb,
                                                                float controlDb,
                                                                float minDb) noexcept {
    const bool reportIsSilence = reportedDb <= minDb + kLevelEpsilonDb;
    if (muted) {
        // Our own mute being echoed back is expected and says nothing about the knob.
        if (reportIsSilence) {
            return {};
        }
        return {.clearMute = true, .applyToControl = true};
    }
    const float delta = reportedDb > controlDb ? reportedDb - controlDb : controlDb - reportedDb;
    return {.clearMute = false, .applyToControl = delta >= kLevelEpsilonDb};
}

} // namespace ASFW::Audio::DriverKit
