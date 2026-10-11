// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// HostOutputGain.hpp - Master output volume applied to host PCM before it reaches the wire.
//
// For a device whose CoreAudio volume is realised in the host (the profile's
// HostOutputVolume), the volume keys scale the samples CoreAudio wrote; the device receives
// no command.
// vendor driver: the official MOTU driver does the same on the 828mk3 -- with "Enable Core
// Audio volume controls" on, the 828's meters follow the volume keys while its MONITOR
// level and LCD stay put, and no async write reaches the device.
//
// The stage lives in the audio layer, on the HAL output ring, so the transport and the wire
// codecs see ordinary PCM. Threads: the control side may set the target from any thread;
// Apply runs only on CoreAudio's IO thread (WriteEnd), which owns the ramp state.

#pragma once

#include "DecibelMath.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

/// The gain for a level control at `decibels`. The control's floor is silence rather than
/// its nominal dB, so the slider all the way down plays nothing.
[[nodiscard]] constexpr float HostOutputGainFromDecibels(float decibels,
                                                         float minDecibels) noexcept {
    if (!(decibels > minDecibels)) { // also catches NaN
        return 0.0f;
    }
    if (decibels >= 0.0f) {
        return 1.0f;
    }
    return DecibelsToAmplitude(decibels);
}

class HostOutputGain final {
public:
    /// Frames a change takes to reach its target: 10 ms at 48 kHz. A step in one sample
    /// clicks; a ramp this short still follows a held volume key without lag.
    static constexpr uint32_t kRampFrames = 480;

    /// Control side, any thread. Takes effect from the next WriteEnd.
    void SetTarget(float gain) noexcept {
        if (!(gain >= 0.0f)) {
            gain = 0.0f;
        }
        targetBits_.store(__builtin_bit_cast(uint32_t, gain), std::memory_order_release);
    }

    [[nodiscard]] float Target() const noexcept {
        return __builtin_bit_cast(float, targetBits_.load(std::memory_order_acquire));
    }

    /// Control side, any thread. Mute silences the output through the same ramp as a
    /// volume change, so it does not click; the volume target is kept, and unmuting
    /// returns to it.
    void SetMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_release); }

    [[nodiscard]] bool Muted() const noexcept { return muted_.load(std::memory_order_acquire); }

    /// IO thread only. Scales `frameCount` interleaved frames starting at absolute frame
    /// `firstFrame` of a ring of `capacityFrames` frames, in place. RT-safe: no locks, no
    /// allocation. At unity with no ramp in progress it does not touch the ring.
    void Apply(float* ring, uint32_t channels, uint32_t capacityFrames, uint64_t firstFrame,
               uint32_t frameCount) noexcept {
        if (ring == nullptr || channels == 0 || capacityFrames == 0) {
            return;
        }
        const float target = Muted() ? 0.0f : Target();
        if (target != rampTarget_) {
            rampTarget_ = target;
            rampLeft_ = kRampFrames;
            step_ = (target - current_) / static_cast<float>(kRampFrames);
        }
        if (rampLeft_ == 0 && current_ == 1.0f) {
            return;
        }

        uint32_t index = static_cast<uint32_t>(firstFrame % capacityFrames);
        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            if (rampLeft_ != 0) {
                --rampLeft_;
                current_ = rampLeft_ == 0 ? rampTarget_ : current_ + step_;
            }
            float* sample = ring + (static_cast<uint64_t>(index) * channels);
            for (uint32_t channel = 0; channel < channels; ++channel) {
                sample[channel] *= current_;
            }
            if (++index == capacityFrames) {
                index = 0;
            }
        }
    }

    /// IO-thread view of the gain the last frame was scaled by.
    [[nodiscard]] float Current() const noexcept { return current_; }

private:
    std::atomic<uint32_t> targetBits_{__builtin_bit_cast(uint32_t, 1.0f)};
    std::atomic<bool> muted_{false};
    float current_{1.0f};
    float rampTarget_{1.0f};
    float step_{0.0f};
    uint32_t rampLeft_{0};
};

} // namespace ASFW::Audio::Runtime
