// TxPacketizerTestSupport.hpp -- frame packets the way DiceTxStreamEngine does.
//
// The packetizer keeps no audio-frame cursor (documentation/TX_OWNERSHIP.md):
// its caller decides DATA vs NO-DATA and the frames of each packet, and owns
// the frame cursor. Tests that drive the packetizer directly use this helper,
// which mirrors DiceTxStreamEngine::PrepareNextTransmitSlot's framing step,
// with the frame cursor held by the test.
#pragma once

#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"

#include <cstdint>

namespace ASFW::Testing {

inline bool PrepareCadencePacket(ASFW::Protocols::Audio::AMDTP::AmdtpTxPacketizer& packetizer,
                                 ASFW::Protocols::Audio::AMDTP::TxPacketSlotView slot,
                                 const ASFW::Protocols::Audio::AMDTP::AmdtpTimingState& timing,
                                 uint64_t& nextAudioFrame,
                                 ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& out) {
    using ASFW::Protocols::Audio::AMDTP::AmdtpPacketDisposition;
    const bool cadenceData = packetizer.NextPacketWouldCarryData();
    const bool isData = timing.disposition == AmdtpPacketDisposition::Data &&
                        (timing.replayValid ? timing.replayDataBlocks != 0 : cadenceData);
    const uint8_t frames =
        isData ? static_cast<uint8_t>(timing.replayValid ? timing.replayDataBlocks
                                                         : packetizer.CurrentCycleDataFrames())
               : 0;
    ASFW::Protocols::Audio::AMDTP::TxPresentationPlan plan{};
    plan.cycleOrdinal = slot.packetIndex;
    plan.firstAudioFrame = nextAudioFrame;
    plan.frameCount = frames;
    plan.disposition = isData ? AmdtpPacketDisposition::Data : AmdtpPacketDisposition::NoData;
    if (!packetizer.PrepareNextPacket(slot, timing, plan, out)) {
        return false;
    }
    if (out.isData) {
        nextAudioFrame += out.framesInPacket;
    }
    return true;
}

} // namespace ASFW::Testing
