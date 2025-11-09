#pragma once

#include "ARContextBase.hpp"

namespace ASFW::Async {

/**
 * \brief OHCI AR (Asynchronous Receive) Response context.
 *
 * Receives incoming asynchronous response packets (read response, write response,
 * lock response) from the IEEE 1394 bus. These are replies to requests sent
 * by local AT contexts.
 *
 * \par OHCI Specification References
 * - §8.2: AsRspRcvContextControl registers (0x420-0x42C)
 * - §8.4.2: AR DMA operation in buffer-fill mode
 *
 * \par Design Rationale
 * AR Response context is simpler than AR Request because:
 * - **No PHY packets**: Only receives normal async responses
 * - **No bus-reset packets**: Synthetic bus-reset packet only goes to AR Request
 * - **Bus reset flexibility**: MAY be stopped during reset (though usually kept running)
 *
 * Received responses are matched to pending AT requests by tLabel (transaction label).
 * The PacketRouter dispatches responses to handlers registered by tCode.
 *
 * \par Apple Pattern
 * Based on AppleFWOHCI_AsyncReceiveResponse (AsyncReceive.cpp):
 * - Receives async responses only (no PHY packets)
 * - Typically kept running during bus reset
 * - Responses matched to pending requests via tLabel
 *
 * \par Linux Pattern
 * See drivers/firewire/ohci.c:
 * - ar_context_init(&ohci->ar_response_ctx): Initializes response context
 * - handle_ar_packet(): Processes responses and completes pending requests
 * - bus_reset_work(): AR Response may be temporarily gated but usually continues
 *
 * \par Register Map (OHCI §8.2)
 * - 0x420: AsRspRcvContextControlSet
 * - 0x424: AsRspRcvContextControlClear
 * - 0x428: (reserved)
 * - 0x42C: AsRspRcvCommandPtr
 *
 * \par Response Matching
 * Each response contains:
 * - tLabel: 6-bit transaction label (matches pending AT request)
 * - tCode: Transaction code (read response, write response, etc.)
 * - sourceID: Node that sent response
 *
 * The response handler must match tLabel to pending request and complete
 * the transaction (either success or error based on rCode in response).
 */
class ARResponseContext : public ARContextBase<ARResponseContext, ARResponseTag> {
public:
    using RoleTag = ARResponseTag;

    ARResponseContext() = default;
    ~ARResponseContext() = default;

    ARResponseContext(const ARResponseContext&) = delete;
    ARResponseContext& operator=(const ARResponseContext&) = delete;
};

} // namespace ASFW::Async
