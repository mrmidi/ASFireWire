// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV3PayloadWriter.hpp - Host->device PCM placement for MOTU protocol-v3 (828 Mk3).
//
// The v3 data block is the v2 block: SPH quadlet, two message chunks, then 3-byte PCM
// chunks from byte 10. Addressing a sample as `kSphBytes + wireChunk * 3`, with
// wireChunk counting the message chunks, is the same byte as
// `kPcmByteOffset + (wireChunk - 2) * 3`. So placement is the shared
// MotuPayloadWriter unchanged, driven by the Mk3 port map (kMk3Playback); the tests pin
// it bit-for-bit against an independent reference encoder.
//
// What v3 adds is the acquisition mute: while the SPH servo has not locked phase,
// packets stay DATA but carry silence. Muting must not turn them into NO-DATA -- the
// device would see the stream stop. The servo drives SetPcmMuted() from the TX
// producer; this writer only honours it and counts the frames
// (framesIntentionallyMuted).
//
// Every call is also handed to MotuV3TxSilenceDiagnostics, which says
// where silence on the 828 comes from: the host, the mute, the placement or the packet.

#pragma once

#include "MotuPayloadWriter.hpp"
#include "MotuPortLayout.hpp"
#include "MotuV3TxSilenceDiagnostics.hpp"
#include "../AMDTP/MotuV3WireFormat.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Wire {

namespace MotuV3PayloadWriterDetail {
[[nodiscard]] constexpr bool PortMapMatchesCapturedWireOrder() noexcept {
    using ::ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kCoreOutputChannels;
    using ::ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kCoreToWireChunk;
    using ::ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kMessageChunks;
    constexpr auto& ports = ::ASFW::Encoding::Motu::kMk3Playback;
    if (sizeof(ports) / sizeof(ports[0]) != kCoreOutputChannels) {
        return false;
    }
    for (size_t host = 0; host < kCoreOutputChannels; ++host) {
        if (ports[host].chunk + kMessageChunks != kCoreToWireChunk[host]) {
            return false;
        }
    }
    return true;
}
static_assert(PortMapMatchesCapturedWireOrder(),
              "kMk3Playback must be the captured CoreAudio-to-wire order");
static_assert(::ASFW::Encoding::Motu::kPcmByteOffset ==
                  ::ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kSphBytes +
                      ::ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kMessageChunks *
                          ::ASFW::Protocols::Audio::AMDTP::MotuV3Wire::kBytesPerChunk,
              "v3 and v2 must place PCM at the same block offset");
} // namespace MotuV3PayloadWriterDetail

class MotuV3PayloadWriter final : public ::ASFW::Audio::ITxPayloadWriter {
public:
    MotuV3PayloadWriter() noexcept = default;

    void Configure(const ::ASFW::Encoding::Motu::MotuPayloadStreamConfig& streamConfig) noexcept {
        placement_.Configure(streamConfig);
        diagnostics_.Configure(streamConfig);
    }

    void BindTimeline(Protocols::Audio::AMDTP::AmdtpPacketTimeline* timeline) noexcept {
        placement_.BindTimeline(timeline);
        diagnostics_.BindTimeline(timeline);
    }

    /// Set by the SPH servo before the payload fill that follows. Release/acquire so
    /// the writer never mixes a decision with the samples of the one before it.
    void SetPcmMuted(bool muted) noexcept {
        muted_.store(muted, std::memory_order_release);
    }

    [[nodiscard]] bool IsPcmMuted() const noexcept {
        return muted_.load(std::memory_order_acquire);
    }

    void WriteFloat32Interleaved(
        const Protocols::Audio::AMDTP::HostAudioBufferView& hostBuffer,
        uint64_t firstWritablePacket) noexcept override {
        const MotuV3PlacementDelta before = PlacementTotals();
        const bool muted = muted_.load(std::memory_order_acquire);
        if (!muted) {
            placement_.WriteFloat32Interleaved(hostBuffer, firstWritablePacket);
        } else {
            // Same frames, same packets, same finality frontier -- only the source is
            // silence. A one-channel, one-frame ring of zero serves every frame (the
            // placement reads frame `absolute % 1`), and channels past it encode zero.
            static constexpr float kSilence = 0.0f;
            Protocols::Audio::AMDTP::HostAudioBufferView silent = hostBuffer;
            silent.interleavedFloat32 =
                hostBuffer.interleavedFloat32 != nullptr ? &kSilence : nullptr;
            silent.frameCapacity = 1;
            silent.channels = hostBuffer.channels != 0 ? 1U : 0U;
            placement_.WriteFloat32Interleaved(silent, firstWritablePacket);
        }
        const MotuV3PlacementDelta after = PlacementTotals();
        const MotuV3PlacementDelta placed{
            .written = after.written - before.written,
            .withoutPacket = after.withoutPacket - before.withoutPacket,
            .outsidePacket = after.outsidePacket - before.outsidePacket,
            .missedFinality = after.missedFinality - before.missedFinality,
            .truncated = after.truncated - before.truncated,
        };
        if (muted) {
            framesIntentionallyMuted_.fetch_add(placed.written, std::memory_order_relaxed);
        }
        // The real host buffer, also when muted: the question is whether the host
        // fed signal that the mute then held back.
        diagnostics_.Observe(hostBuffer, firstWritablePacket, muted, placed);
    }

    [[nodiscard]] const ::ASFW::Encoding::Motu::MotuPayloadWriterCounters& Counters() const noexcept {
        return placement_.Counters();
    }

    /// Frames written as silence because the servo held the mute. Included in
    /// Counters().framesWritten.
    [[nodiscard]] uint64_t FramesIntentionallyMuted() const noexcept {
        return framesIntentionallyMuted_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const MotuV3TxSilenceCounters& SilenceCounters() const noexcept {
        return diagnostics_.Counters();
    }

private:
    // Read by the one thread that also writes them, so the difference across a
    // call is exact.
    [[nodiscard]] MotuV3PlacementDelta PlacementTotals() const noexcept {
        const auto& c = placement_.Counters();
        return {
            .written = c.framesWritten.load(std::memory_order_relaxed),
            .withoutPacket = c.framesWithoutPacket.load(std::memory_order_relaxed),
            .outsidePacket = c.framesOutsidePacket.load(std::memory_order_relaxed),
            .missedFinality = c.framesMissedFinality.load(std::memory_order_relaxed),
            .truncated = c.framesTruncated.load(std::memory_order_relaxed),
        };
    }

    ::ASFW::Encoding::Motu::MotuPayloadWriter placement_{};
    MotuV3TxSilenceDiagnostics diagnostics_{};
    std::atomic<bool> muted_{false};
    std::atomic<uint64_t> framesIntentionallyMuted_{0};
};

} // namespace ASFW::Audio::Wire
