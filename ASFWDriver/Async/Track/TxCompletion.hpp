// TxCompletion.hpp

#pragma once

#include <cstdint>
#include <cstddef>
#include "OHCIEventCodes.hpp"
#include "OHCIDescriptors.hpp"

namespace ASFW::Async {

/**
 * \brief Transmit completion result from ScanCompletion().
 *
 * Contains hardware-reported status and timestamp for a completed AT descriptor.
 */
struct TxCompletion {
    OHCIEventCode eventCode{OHCIEventCode::kEvtNoStatus};  ///< Extracted from xferStatus[4:0]
    uint16_t timeStamp{0};                                  ///< Cycle timer snapshot
    uint8_t ackCount{0};                                    ///< Transmission attempts from xferStatus[7:5]
    uint8_t ackCode{0};                                     ///< IEEE 1394 ACK code from xferStatus[15:12]
    uint8_t tLabel{0xFF};                                   ///< Transaction label (0-63) or 0xFF if unavailable
    HW::OHCIDescriptor* descriptor{nullptr};                ///< Completed descriptor pointer
    bool isResponseContext{false};                          ///< True if completion came from AT Response context (WrResp)
};

} // namespace ASFW::Async
