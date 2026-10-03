// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RxInputContentMeter.hpp - Per-channel peak of what RX wrote to the input buffer.
//
// RX telemetry counts packets and frames, never sample content, so "the device
// sends audio but the application hears silence" could not be split at the
// driver boundary. This meter reads the frames the decoder
// has just written, after the capture channel map, i.e. exactly what CoreAudio
// will be handed, and reports once per window:
//
//   [RxInputMeter] off=0 ch=18 frames=96000 sig=0x00003 pk=-12,-15,-,-,...
//
// `sig` has bit n set when channel n carried signal; `pk` is each channel's peak
// in whole dBFS, rounded down to the next lower dB step (0 at or above full
// scale), or `-` for silence. "Signal" is the TX diagnostics' criterion
// (MotuV3TxSilenceDiagnostics.hpp): a sample that encodes to a non-zero 24-bit
// value. Since encoding is monotonic, a channel has signal exactly when its peak
// does, so the per-sample cost is one absolute value and one compare.
//
// Counterpart of tools/motu_input_meter.swift, which sees the same thing from an
// application but must start IO and needs a microphone grant; this one is live
// and passive.
//
// One writer: the RX consumer's packet thread. Formatting happens once per
// window, not per packet. DriverKit has no libm, so dB comes from a constexpr
// threshold table instead of log10.

#pragma once

#include "../../../Wire/AMDTP/PcmSlotCodec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ASFW::AudioEngine::Direct::Rx {

namespace detail {

/// 10^(-db/20) for db = 0..Steps-1: the peak at or above which a channel
/// reads -db dBFS.
template <uint32_t Steps>
constexpr std::array<float, Steps> MakeDbThresholds() noexcept {
    std::array<float, Steps> table{};
    double level = 1.0;
    for (uint32_t db = 0; db < Steps; ++db) {
        table[db] = static_cast<float>(level);
        level *= 0.8912509381337456; // 10^(-1/20)
    }
    return table;
}

} // namespace detail

class RxInputContentMeter final {
public:
    /// Two seconds at 48 kHz: the cadence of [TxChunkMap], so the TX and RX
    /// lines of one run interleave evenly in the ring.
    static constexpr uint32_t kWindowFrames = 96000;
    /// More than the 18 inputs of the 828 Mk3; the rest are reported as `ch=`.
    static constexpr uint32_t kMaxChannels = 32;
    /// The 24-bit LSB is -138.5 dBFS, so every encodable signal lands in the table.
    static constexpr uint32_t kDbSteps = 139;

    /// Feeds one frame of `channels` samples. Returns true when the window closed;
    /// the line is then ready in Line() until the next frame.
    bool ObserveFrame(const float* frame, uint32_t channels) noexcept {
        if (frame == nullptr || channels == 0) {
            return false;
        }
        if (channels != channels_) {
            Reset(channels);
        }
        const uint32_t tracked = channels < kMaxChannels ? channels : kMaxChannels;
        for (uint32_t ch = 0; ch < tracked; ++ch) {
            const float magnitude = frame[ch] < 0.0F ? -frame[ch] : frame[ch];
            if (magnitude > peaks_[ch]) {
                peaks_[ch] = magnitude;
            }
        }
        if (++frames_ < kWindowFrames) {
            return false;
        }
        Format();
        Reset(channels);
        return true;
    }

    void Reset(uint32_t channels) noexcept {
        channels_ = channels;
        frames_ = 0;
        peaks_.fill(0.0F);
    }

    /// The line formatted when the last window closed, without the [RxInputMeter]
    /// marker, which the caller's format carries so grep and CodeGraph find it.
    [[nodiscard]] const char* Line() const noexcept { return line_.data(); }
    [[nodiscard]] uint32_t SignalMask() const noexcept { return signalMask_; }

    /// Whole dBFS step for a peak, or -1 when it encodes to zero (silence).
    [[nodiscard]] static int PeakDb(float peak) noexcept {
        if (::ASFW::Protocols::Audio::AMDTP::PcmSlotCodec::Float32ToSigned24(peak) == 0) {
            return -1;
        }
        static constexpr auto kThresholds = detail::MakeDbThresholds<kDbSteps>();
        for (uint32_t db = 0; db < kDbSteps; ++db) {
            if (peak >= kThresholds[db]) {
                return static_cast<int>(db);
            }
        }
        return static_cast<int>(kDbSteps);
    }

private:
    void Format() noexcept {
        const uint32_t tracked = channels_ < kMaxChannels ? channels_ : kMaxChannels;
        signalMask_ = 0;
        std::array<char, 160> peaks{};
        size_t used = 0;
        for (uint32_t ch = 0; ch < tracked && used + 6 < peaks.size(); ++ch) {
            const int db = PeakDb(peaks_[ch]);
            if (db >= 0) {
                signalMask_ |= 1U << ch;
            }
            const int written =
                db < 0 ? snprintf(peaks.data() + used, peaks.size() - used, "%s-",
                                  ch == 0 ? "" : ",")
                       : snprintf(peaks.data() + used, peaks.size() - used, "%s%d",
                                  ch == 0 ? "" : ",", db == 0 ? 0 : -db);
            if (written <= 0) {
                break;
            }
            used += static_cast<size_t>(written);
        }
        snprintf(line_.data(), line_.size(), "ch=%u frames=%u sig=0x%05x pk=%s", channels_,
                 frames_, signalMask_, peaks.data());
    }

    uint32_t channels_{0};
    uint32_t frames_{0};
    uint32_t signalMask_{0};
    std::array<float, kMaxChannels> peaks_{};
    std::array<char, 200> line_{};
};

} // namespace ASFW::AudioEngine::Direct::Rx
