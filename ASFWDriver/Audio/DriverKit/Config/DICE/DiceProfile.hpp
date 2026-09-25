// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfile.hpp - The one DICE audio profile.
//
// Every DICE device is described the same way: its stream geometry, rates,
// clock and channel names come from its own registers (DICE_TCAT_ARCHITECTURE.md
// §4.1), so a profile keeps only what the device cannot report. That is a small
// spec per catalog builder -- a name, the host framing policy, and the one
// geometry fact backed by a reference stack -- instead of the seven per-model
// classes it replaces (stage C of §4.2). The vendor kexts carry no per-model
// geometry either: all five TCAT kexts are one codebase.

#pragma once

#include "DiceDeviceProfile.hpp"
#include "../TimingLadder.hpp"

#include <cstdint>
#include <span>

namespace ASFW::Isoch::Audio::DICE {

// A range member named by its capture width, e.g. the Midas Venice F16/F24/F32:
// one identity, and the model number is the capture channel count.
struct DiceRangeMember {
    uint32_t captureChannels{0};
    const char* name{nullptr};
};

// What a DICE device cannot report about itself.
struct DiceProfileSpec {
    const char* name{nullptr};
    // Names for members of a range that share one identity.
    std::span<const DiceRangeMember> rangeMembers{};
    // Host-to-device PCM: raw 24-in-32, as every TCAT kext sends it, or AM824.
    Encoding::AudioWireFormat txEncoding{Encoding::AudioWireFormat::kRawPcm24In32};
    bool preserveFdfInNoDataPackets{true};
    bool initializeNonAudioSlots{true};
    // Playback streams the device really has, when a reference stack says the
    // register overstates it (libffado dice_avdevice.cpp:1686-1700: Alesis
    // model 0 "announces two receive transmitters, but only has one").
    // Zero: the device's count stands.
    uint32_t assertedPlaybackStreams{0};
    // Capture safety offset in packets; the vendor ladder's 16 unless measured.
    uint32_t captureSafetyPackets{TimingLadder::kRxDelayPackets};
    // Measured device latency at 1x, doubling per rate tier. Zero keeps the
    // vendor ladder (29/59/119).
    uint32_t inputLatency1x{0};
    uint32_t outputLatency1x{0};
};

class DiceProfile final : public IDiceDeviceProfile {
public:
    explicit constexpr DiceProfile(DiceProfileSpec spec) noexcept : spec_(spec) {}

    [[nodiscard]] const char* Name() const noexcept override { return spec_.name; }
    [[nodiscard]] const char* NameForGeometry(uint32_t hostInputPcmChannels,
                                              uint32_t hostOutputPcmChannels) const noexcept override;
    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;

    // Framing constants only: stream shape and counts come from the device.
    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;

    // Playback streams to insist on, or zero to take the device's count.
    [[nodiscard]] uint32_t AssertedPlaybackStreams() const noexcept {
        return spec_.assertedPlaybackStreams;
    }

private:
    DiceProfileSpec spec_;
};

} // namespace ASFW::Isoch::Audio::DICE
