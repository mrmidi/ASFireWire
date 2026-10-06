// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GenericAvcProfile.cpp - The one audio profile for AV/C units published from
// generic discovery.

#include "GenericAvcProfile.hpp"
#include "../../../../Protocols/AVC/Commands/GeneralCommands.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

namespace {

void FillFraming(AudioStreamConfig& out, AudioStreamDirection direction) noexcept {
    // Blocking AM824 at 8 frames per packet, FMT 0x10. Channel and slot counts
    // stay zero; the published geometry fills them in
    // (BuildResolvedTxStreamConfig), and the packetizer takes the FDF from the
    // live rate.
    out = AudioStreamConfig{};
    out.direction = direction;
    out.sampleRate = 48000;
    out.streamMode = Encoding::StreamMode::kBlocking;
    out.sid = 0;
    out.pcmChannels = 0;
    out.midiSlots = 0;
    out.dbs = 0;
    out.framesPerDataPacket = 8;
    // Placeholder FDF for 48 kHz (IEC 61883-6 Table 20, SFC 2); the packetizer takes the FDF from the live rate.
    out.fdf = static_cast<uint8_t>(::ASFW::AVC::CipSfc::k48000);
    out.fmt = ::ASFW::AVC::Cmd::kFmtAudioMusic;
}

} // namespace

bool GenericAvcProfile::BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    FillFraming(outConfig, AudioStreamDirection::HostToDevice);
    return true;
}

bool GenericAvcProfile::BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    FillFraming(outConfig, AudioStreamDirection::DeviceToHost);
    return true;
}

// Safety offsets and latencies, field-tuned on the Onyx-i 2026-08-17: the
// earlier 64/128 values produced audible in-and-out playback on an 8 GB M3
// (host callback latency spiked to ~922 us against a 64-frame TX safety
// offset). The wider values trade ~3 ms of reported latency for jitter
// headroom, and apply to every AV/C device on this path.
// Preserve the validated 48 kHz scheduling budget in time at higher rates.
// A fixed 192-frame TX lead shrinks from 4 ms to 2 ms at 96 kHz and 1 ms
// at 192 kHz, although the host scheduling delays do not shrink with it.
// Keep the existing low-rate floors and presentation-latency declarations;
// scheduling safety is independent of converter/stream latency.
namespace {
uint32_t SchedulingFrames(uint32_t baselineFrames, double rate) noexcept {
    if (rate <= 48000.0) {
        return baselineFrames;
    }
    const auto hz = static_cast<uint32_t>(rate);
    return static_cast<uint32_t>((static_cast<uint64_t>(baselineFrames) * hz + 47999U) /
                                 48000U);
}
} // namespace

uint32_t GenericAvcProfile::TxSafetyOffsetFrames(double rate) const noexcept {
    return SchedulingFrames(192, rate);
}
uint32_t GenericAvcProfile::RxSafetyOffsetFrames(double rate) const noexcept {
    return SchedulingFrames(256, rate);
}
uint32_t GenericAvcProfile::TxReportedLatencyFrames(double) const noexcept { return 256; }
uint32_t GenericAvcProfile::RxReportedLatencyFrames(double) const noexcept { return 256; }

AudioStreamTxPolicy GenericAvcProfile::TxStreamPolicy() const noexcept {
    // NO-DATA packets while output is idle. Without them a record-only session
    // leaves the output ring unfed and the transmit deficit grows until the
    // session collapses (Onyx-i, field-verified 2026-08-17); the PHASE 88 ran
    // this policy from the start.
    return AudioStreamTxPolicy{
        .hostToDevicePcmEncoding = Encoding::AudioWireFormat::kAM824,
        .variableDbs = false,
        .defaultNonAudioSlotWord = 0x80000000,
        .initializeNonAudioSlots = true,
        .preserveFdfInNoDataPackets = false,
        .emptyPacketsDuringIdle = true,
    };
}

} // namespace ASFW::Isoch::Audio::AVC::Profiles
