#pragma once

#include "ARContextBase.hpp"

namespace ASFW::Async {

/**
 * \brief OHCI AR (Asynchronous Receive) Request context.
 *
 * Receives incoming asynchronous request packets (read, write, lock) from
 * the IEEE 1394 bus. Also receives PHY packets and synthetic bus-reset packets.
 *
 * \par OHCI Specification References
 * - §8.2: AsReqRcvContextControl registers (0x400-0x40C)
 * - §8.4.2: AR DMA operation in buffer-fill mode
 * - §8.4.2.3: PHY packet reception when LinkControl.rcvPhyPkt=1
 * - §C.3: Bus reset handling (AR Request MUST stay running)
 *
 * \par Special Behavior
 * Unlike AR Response context, AR Request has special responsibilities:
 *
 * 1. **PHY Packet Reception**: When LinkControl.rcvPhyPkt=1, AR Request
 *    receives PHY packets (tCode=0xE) in addition to normal async requests.
 *    These include link-on and self-ID packets.
 *
 * 2. **Bus Reset Packets**: Per OHCI §C.3, when a bus reset occurs, OHCI
 *    generates a synthetic "bus reset packet" and delivers it to AR Request
 *    context. This packet has a special format distinct from normal packets.
 *
 * 3. **Continuous Operation**: AR Request MUST NOT be stopped during bus
 *    reset. It must keep running to receive the bus-reset packet and any
 *    subsequent PHY packets during topology discovery.
 *
 * \par Apple Pattern
 * Based on AppleFWOHCI_AsyncReceiveRequest (AsyncReceive.cpp):
 * - Receives all async requests plus PHY packets
 * - Processes bus-reset packets in interrupt handler
 * - Never stopped during bus reset sequence
 *
 * \par Linux Pattern
 * See drivers/firewire/ohci.c:
 * - ar_context_init(&ohci->ar_request_ctx): Initializes request context
 * - handle_ar_packet(): Processes requests and PHY packets
 * - Bus reset: AR Request context continues running (unlike AT contexts)
 *
 * \par Register Map (OHCI §8.2)
 * - 0x400: AsReqRcvContextControlSet
 * - 0x404: AsReqRcvContextControlClear
 * - 0x408: (reserved)
 * - 0x40C: AsReqRcvCommandPtr
 *
 * \warning Never call Stop() on AR Request during bus reset - this violates
 *          OHCI §C.3 and will prevent reception of bus-reset/PHY packets!
 */
class ARRequestContext : public ARContextBase<ARRequestContext, ARRequestTag> {
public:
    using RoleTag = ARRequestTag;

    ARRequestContext() = default;
    ~ARRequestContext() = default;

    ARRequestContext(const ARRequestContext&) = delete;
    ARRequestContext& operator=(const ARRequestContext&) = delete;
};

} // namespace ASFW::Async
