#include "DiceStreamConfig.hpp"

namespace ASFW::Protocols::Audio::DICE {

AMDTP::AmdtpStreamConfig DiceStreamConfigMapper::ToAmdtpConfig(
    const DiceStreamConfig& diceConfig) noexcept {
    AMDTP::AmdtpStreamConfig config{};
    config.sampleRate = diceConfig.sampleRate;
    config.streamMode = diceConfig.streamMode;
    config.sid = diceConfig.sid;
    config.dbs = diceConfig.dbs;
    config.pcmChannels = diceConfig.pcmChannels;
    config.midiSlots = diceConfig.midiSlots;
    config.fmt = diceConfig.fmt;
    config.fdf = diceConfig.fdf;
    config.framesPerDataPacket = diceConfig.framesPerDataPacket;
    // maxPacketBytes keeps the AmdtpStreamConfig default (512), matching the
    // lab slot capacity and the ASFW IT ring payload budget.
    return config;
}

} // namespace ASFW::Protocols::Audio::DICE
