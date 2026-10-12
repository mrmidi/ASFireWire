// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// HostOutputVolumePolicy.hpp - Whether a host-realised master volume is published to CoreAudio.
//
// vendor driver: the official MOTU driver publishes its CoreAudio volume only while "Enable Core
// Audio volume controls" is checked in MOTU Audio Setup; unchecked, macOS shows no volume, the
// volume keys do nothing, and the level is the device's own MONITOR.
//
// ASFW keeps one such setting for the MOTU family. It governs a volume the profile realises in
// the host (HostOutputVolume, today the 828mk3's). Off publishes no volume at all rather than
// falling back to a device control: on the 828mk3 that control would be the MONITOR level, and
// the volume keys would turn the device's knob. The app sends the setting on every connection;
// until then the driver keeps its default, on.

#pragma once

#include "HostOutputGain.hpp"

#include <atomic>

namespace ASFW::Audio::Runtime {

/// The user's choice, process-wide: the controller's user client writes it and every audio
/// driver in the dext reads it.
class CoreAudioVolumeSetting final {
public:
    static CoreAudioVolumeSetting& Shared() noexcept {
        static CoreAudioVolumeSetting instance;
        return instance;
    }

    void SetEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_release); }

    [[nodiscard]] bool Enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> enabled_{true};
};

/// With its controls withdrawn the gain stage must stop attenuating: the output returns,
/// through the usual ramp, to the level set on the device, and a mute left on is released.
inline void ReleaseHostOutputGain(HostOutputGain& gain) noexcept {
    gain.SetMuted(false);
    gain.SetTarget(1.0f);
}

} // namespace ASFW::Audio::Runtime
