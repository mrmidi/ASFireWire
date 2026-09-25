// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceProfile.cpp - The one DICE audio profile.

#include "DiceProfile.hpp"


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

// Safety offsets and latency follow the packet-scaled ladder of Focusrite's
// Saffire.kext (TimingLadder), which the per-model DICE classes carried in three
// spellings. A spec may replace the capture safety and the latency with values
// measured on the device.
namespace {

[[nodiscard]] constexpr uint32_t PerTier(uint32_t at1x, double sampleRate) noexcept {
    return TimingLadder::FramesPerPacket(sampleRate) == 0
               ? 0U
               : at1x << TimingLadder::RateTier(sampleRate);
}
static_assert(PerTier(53, 48000.0) == 53);
static_assert(PerTier(53, 96000.0) == 106);
static_assert(PerTier(52, 192000.0) == 208);

} // namespace

uint32_t DiceProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kTxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

uint32_t DiceProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(spec_.captureSafetyPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

uint32_t DiceProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    return spec_.outputLatency1x != 0 ? PerTier(spec_.outputLatency1x, sampleRate)
                                      : TimingLadder::ReportedLatencyFrames(sampleRate);
}

uint32_t DiceProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    return spec_.inputLatency1x != 0 ? PerTier(spec_.inputLatency1x, sampleRate)
                                     : TimingLadder::ReportedLatencyFrames(sampleRate);
}

} // namespace ASFW::Isoch::Audio::DICE
