// PreSonusStudioLiveProfile.cpp
// PreSonus StudioLive 16.0.2 FireWire profile (DICE/TCAT).
//
// StudioLive 16.0.2: 16-channel mixer with a 16-in/16-out FireWire interface.
// Stream geometry captured live from the hardware (2026-07-08) via the DICE
// register space at 48 kHz: a single isochronous stream per direction with
// NB_AUDIO=16, NB_MIDI=1 (DBS = 17), S400. Clock capabilities register
// (0x13000006) advertises 44.1/48 kHz only — no mid/high rate modes — so the
// low-rate geometry below covers every rate the device supports.
// Cross-checked with Linux snd-dice (fully generic DICE path, no PreSonus
// StudioLive quirks) and libffado 2.5.0 (vendor 0x000a92, model 0x000013).

#include "PreSonusStudioLiveProfile.hpp"
#include "../../../TimingLadder.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {


constexpr uint32_t kPcmChannels = 16;
constexpr uint32_t kMidiSlots   = 1;
constexpr uint32_t kDbs         = kPcmChannels + kMidiSlots;

void FillStreamConfig(DiceStreamConfig& out, DiceStreamDirection direction) noexcept {
    out = DiceStreamConfig{};
    out.direction    = direction;
    out.sampleRate   = 48000;
    out.streamMode   = Encoding::StreamMode::kBlocking;
    out.sid          = 0;
    out.framesPerDataPacket = 8;
    out.fdf          = 0x02;
    out.fmt          = 0x10;
    out.pcmChannels  = kPcmChannels;
    out.midiSlots    = kMidiSlots;
    out.dbs          = kDbs;
}

} // namespace

const char* PreSonusStudioLiveProfile::Name() const noexcept {
    return "PreSonus StudioLive 16.0.2 (DICE)";
}

DiceDeviceQuirks PreSonusStudioLiveProfile::Quirks() const noexcept {
    DiceDeviceQuirks quirks{};
    // Same TCAT DICE chip family as Focusrite Saffire / Midas Venice — same wire encoding.
    quirks.tx.hostToDevicePcmEncoding  = Encoding::AudioWireFormat::kRawPcm24In32;
    quirks.tx.dbsPolicy                = DbsPolicy::Constant;
    quirks.tx.defaultNonAudioSlotWord  = 0x80000000;
    quirks.tx.initializeNonAudioSlots  = true;
    quirks.tx.preserveFdfInNoDataPackets = true;
    quirks.rx.deviceToHostPcmEncoding  = Encoding::AudioWireFormat::kAM824;
    quirks.rx.dbsPolicy                = DbsPolicy::Constant;
    return quirks;
}

bool PreSonusStudioLiveProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool PreSonusStudioLiveProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

// Safety offsets follow the Focusrite Saffire baseline (the tested TCAT ladder):
// Tx 6 packets, Rx 16 packets, scaled by frames-per-packet per rate mode. The
// device only advertises the low rate mode (8 frames per packet), so the higher
// branches are defensive.
uint32_t PreSonusStudioLiveProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kTxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

uint32_t PreSonusStudioLiveProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kRxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

uint32_t PreSonusStudioLiveProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    return TimingLadder::ReportedLatencyFrames(sampleRate);
}

uint32_t PreSonusStudioLiveProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    return TimingLadder::ReportedLatencyFrames(sampleRate);
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
