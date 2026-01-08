#pragma once

#include <cstdint>

namespace ASFW::Async {

/**
 * \brief Response codes for AR Request handlers per IEEE 1394-1995 Table 6-3
 *
 * These values match Linux firewire and Apple IOFireWireFamily implementations.
 * Handlers return these codes to indicate success/failure; the AR infrastructure
 * uses them to construct WrResp packets.
 *
 * **Design**: Handlers are protocol-agnostic - they only choose the rCode.
 * The AR infrastructure (PacketRouter/ResponseSender) owns the policy of
 * whether to actually send a WrResp (e.g., skips broadcast destID=0xFFFF).
 */
enum class ResponseCode : uint8_t {
    Complete      = 0x0,  ///< OK - request successfully completed
    ConflictError = 0x4,  ///< Resource conflict, may retry
    DataError     = 0x5,  ///< Data not available / corrupted
    TypeError     = 0x6,  ///< Operation not supported for this address
    AddressError  = 0x7,  ///< Address not valid in this address space
    NoResponse    = 0xFF  ///< Internal sentinel: do not send WrResp (AR Response context)
};

} // namespace ASFW::Async
