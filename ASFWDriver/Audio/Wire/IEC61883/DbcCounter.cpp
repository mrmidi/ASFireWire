#include "DbcCounter.hpp"

namespace ASFW::Protocols::Audio::IEC61883 {

// DBC semantics follow the Linux amdtp default (advance-after-emit, i.e. the
// packet carries the index of its FIRST data block; CIP_DBC_IS_END_EVENT is a
// device quirk we do not implement). Consequence verified against both Linux
// amdtp-stream.c and FFADO AmdtpTransmitStreamProcessor: no-data packets carry
// the unchanged counter (AdvanceDataBlocks(0)).
//
// The counter value held here is always the DBC for the NEXT packet to be
// emitted; call AdvanceDataBlocks(dataBlocks) after emitting a data packet.

void DbcCounter::Reset(uint8_t initialDbc) noexcept {
    dbc_ = initialDbc;
}

uint8_t DbcCounter::Current() const noexcept {
    return dbc_;
}

uint8_t DbcCounter::ValueForNextPacket() const noexcept {
    return dbc_;
}

void DbcCounter::AdvanceDataBlocks(uint8_t dataBlocks) noexcept {
    dbc_ = static_cast<uint8_t>(dbc_ + dataBlocks);
}

} // namespace ASFW::Protocols::Audio::IEC61883
