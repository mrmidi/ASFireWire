// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfile.cpp - The one DICE audio profile.

#include "DiceProfile.hpp"

#include "../../../../Shared/Isoch/AudioGeometryPolicy.hpp"

namespace ASFW::Isoch::Audio::DICE {

namespace {

void FillFraming(DiceStreamConfig& out, DiceStreamDirection direction) noexcept {
    // Blocking AM824 at 8 frames per packet, FDF 0x02 / FMT 0x10: what the DICE
    // registers do not hold. Channel and slot counts are left at zero; the
    // device's resolved geometry fills them in (BuildResolvedTxStreamConfig).
    out = DiceStreamConfig{};
    out.direction = direction;
    out.sampleRate = 48000;
    out.streamMode = Encoding::StreamMode::kBlocking;
    out.sid = 0;
    out.pcmChannels = 0;
    out.midiSlots = 0;
    out.dbs = 0;
    out.framesPerDataPacket = 8;
    out.fdf = 0x02;
    out.fmt = 0x10;
}

} // namespace

const char* DiceProfile::NameForGeometry(uint32_t hostInputPcmChannels,
                                         uint32_t /*hostOutputPcmChannels*/) const noexcept {
    // A range member is named only when its capture width identifies it; any
    // other width keeps the range name rather than rounding to a guess.
    for (const DiceRangeMember& member : spec_.rangeMembers) {
        if (member.captureChannels == hostInputPcmChannels) {
            return member.name;
        }
    }
    return Name();
}

DiceDeviceQuirks DiceProfile::Quirks() const noexcept {
    DiceDeviceQuirks quirks{};
    quirks.tx.hostToDevicePcmEncoding = spec_.txEncoding;
    quirks.tx.dbsPolicy = DbsPolicy::Constant;
    quirks.tx.defaultNonAudioSlotWord = 0x80000000;
    quirks.tx.initializeNonAudioSlots = spec_.initializeNonAudioSlots;
    quirks.tx.preserveFdfInNoDataPackets = spec_.preserveFdfInNoDataPackets;
    quirks.rx.deviceToHostPcmEncoding = Encoding::AudioWireFormat::kAM824;
    quirks.rx.dbsPolicy = DbsPolicy::Constant;
    return quirks;
}

bool DiceProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillFraming(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool DiceProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillFraming(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

// The packet-scaled ladder of Focusrite's Saffire.kext, which the per-model
// DICE classes carried in three spellings of the same numbers.
using Policy = ::ASFW::IsochTransport::AudioGeometryPolicy;

uint32_t DiceProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    return Policy::TxSafetyOffsetFrames(sampleRate);
}

uint32_t DiceProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    return Policy::RxSafetyOffsetFrames(sampleRate);
}

uint32_t DiceProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    return Policy::ReportedLatencyFrames(sampleRate);
}

uint32_t DiceProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    return Policy::ReportedLatencyFrames(sampleRate);
}

} // namespace ASFW::Isoch::Audio::DICE
