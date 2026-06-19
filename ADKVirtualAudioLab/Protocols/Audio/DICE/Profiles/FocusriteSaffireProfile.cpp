#include "FocusriteSaffireProfile.hpp"

namespace ASFW::Protocols::Audio::DICE::Profiles {

namespace {

// Focusrite OUI (IEEE-assigned); appears as the Config-ROM vendor ID on the
// Saffire Pro 24 DSP bench device.
constexpr uint32_t kFocusriteVendorId = 0x00130E;

void FillDefaultStreamConfig(DiceStreamConfig& outConfig,
                             DiceStreamDirection direction) noexcept {
    outConfig = DiceStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = 48000;
    outConfig.streamMode = AMDTP::StreamMode::Blocking;
    outConfig.sid = 0; // host transmits on ch0/SID0; the device answers on ch1
    // Stream shapes confirmed by a Saffire.kext wire capture at 48 kHz:
    // host→device ch0 = 296 B, dbs 9 (8 audio + 1 MIDI); device→host ch1 =
    // 552 B, dbs 17 (16 AM824 audio + 1 MIDI). Empty MIDI slots carry
    // 0x80000000 in both directions.
    if (direction == DiceStreamDirection::HostToDevice) {
        outConfig.pcmChannels = 8;
        outConfig.midiSlots = 1;
        outConfig.dbs = 9;
    } else {
        outConfig.pcmChannels = 16;
        outConfig.midiSlots = 1;
        outConfig.dbs = 17;
    }
    outConfig.framesPerDataPacket = 8; // blocking SYT_INTERVAL at 48 kHz
    outConfig.fdf = 0x02;
    outConfig.fmt = 0x10;
}

} // namespace

const char* FocusriteSaffireProfile::Name() const noexcept {
    return "Focusrite Saffire (DICE)";
}

bool FocusriteSaffireProfile::Matches(
    const DiceDeviceIdentity& identity) const noexcept {
    return identity.vendorId == kFocusriteVendorId;
}

DiceDeviceQuirks FocusriteSaffireProfile::Quirks() const noexcept {
    DiceDeviceQuirks quirks{};
    // Per-direction PCM framing observed on the wire, matching the DICE-branch
    // RawPcm24In32 codec: host→device slots are unlabeled 24-in-32 big-endian;
    // device→host slots carry AM824 labels.
    quirks.tx.hostToDevicePcmEncoding =
        AMDTP::PcmSlotEncoding::RawSigned24In32BE;
    quirks.tx.dbsPolicy = AMDTP::DbsPolicy::Constant;
    quirks.tx.defaultNonAudioSlotWord = 0x80000000;
    quirks.tx.initializeNonAudioSlots = true;
    quirks.rx.deviceToHostPcmEncoding = AMDTP::PcmSlotEncoding::Am824MBLA;
    quirks.rx.dbsPolicy = AMDTP::DbsPolicy::Constant;
    return quirks;
}

bool FocusriteSaffireProfile::BuildDefaultTxStreamConfig(
    DiceStreamConfig& outConfig) const noexcept {
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool FocusriteSaffireProfile::BuildDefaultRxStreamConfig(
    DiceStreamConfig& outConfig) const noexcept {
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

} // namespace ASFW::Protocols::Audio::DICE::Profiles
