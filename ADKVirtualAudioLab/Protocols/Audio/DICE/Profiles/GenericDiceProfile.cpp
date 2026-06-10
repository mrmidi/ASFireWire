#include "GenericDiceProfile.hpp"

namespace ASFW::Protocols::Audio::DICE::Profiles {

namespace {

void FillDefaultStreamConfig(DiceStreamConfig& outConfig,
                             DiceStreamDirection direction) noexcept {
    outConfig = DiceStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = 48000;
    outConfig.streamMode = AMDTP::StreamMode::Blocking;
    outConfig.sid = 0;
    outConfig.pcmChannels = 2;
    outConfig.dbs = 2;
    outConfig.midiSlots = 0;
    outConfig.framesPerDataPacket = 8;
    outConfig.fdf = 0x02;
    outConfig.fmt = 0x10;
}

} // namespace

const char* GenericDiceProfile::Name() const noexcept {
    return "Generic DICE";
}

bool GenericDiceProfile::Matches(
    const DiceDeviceIdentity& identity) const noexcept {
    (void)identity;
    return true; // catch-all fallback
}

DiceDeviceQuirks GenericDiceProfile::Quirks() const noexcept {
    // Spec-shaped defaults (IEC 61883-6 AM824 both directions). Real devices
    // get a dedicated profile once their wire behavior is known.
    return DiceDeviceQuirks{};
}

bool GenericDiceProfile::BuildDefaultTxStreamConfig(
    DiceStreamConfig& outConfig) const noexcept {
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool GenericDiceProfile::BuildDefaultRxStreamConfig(
    DiceStreamConfig& outConfig) const noexcept {
    FillDefaultStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

} // namespace ASFW::Protocols::Audio::DICE::Profiles
