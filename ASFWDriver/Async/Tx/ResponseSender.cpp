#include "ResponseSender.hpp"

#include "../Engine/ContextManager.hpp"
#include "../Contexts/ATResponseContext.hpp"
#include "../Tx/DescriptorBuilder.hpp"
#include "../Tx/Submitter.hpp"
#include "../../Bus/GenerationTracker.hpp"
#include "../../Logging/Logging.hpp"
#include <DriverKit/IOReturn.h>
#include <utility>

namespace ASFW::Async {

ResponseSender::ResponseSender(DescriptorBuilder& builder,
                               Tx::Submitter& submitter,
                               Engine::ContextManager& ctxMgr,
                               Bus::GenerationTracker& generationTracker) noexcept
    : builder_(builder)
    , submitter_(submitter)
    , ctxMgr_(ctxMgr)
    , generationTracker_(generationTracker) {}

void ResponseSender::SendWriteResponse(const ARPacketView& request, ResponseCode rcode) noexcept {
    // Per IEEE 1394, broadcast requests (destID=0xFFFF) do not get responses.
    if (request.destID == 0xFFFF) {
        ASFW_LOG_V3(Async, "ResponseSender: skip WrResp for broadcast destID=0xFFFF");
        return;
    }

    // Only write requests (quadlet/block) receive a WrResp.
    if (request.tCode != 0x0 && request.tCode != 0x1) {
        ASFW_LOG_V3(Async, "ResponseSender: skip WrResp for non-write tCode=0x%x", request.tCode);
        return;
    }

    auto* atRspCtx = ctxMgr_.GetAtResponseContext();
    if (!atRspCtx) {
        ASFW_LOG_ERROR(Async, "ResponseSender: ATResponseContext unavailable, cannot send WrResp");
        return;
    }

    // Get local node ID from GenerationTracker (explicitly set, not relying on OHCI auto-fill)
    const auto busState = generationTracker_.GetCurrentState();
    const uint16_t localNodeID = busState.localNodeID;
    
    // Destination: respond back to the request initiator
    // Source: our local node ID (typically 0xFFC0)
    const uint16_t destID = request.sourceID;
    const uint16_t srcID  = localNodeID;
    const uint8_t  tLabel = static_cast<uint8_t>(request.tLabel & 0x3F);

    // Build WRITE_RESPONSE header in OHCI AT Data format (NOT IEEE 1394 wire format!)
    // 
    // OHCI AT Data format (host byte order, per Linux ohci.h):
    // Quadlet 0: [srcBusID:1][unused:5][speed:3][tLabel:6][rt:2][tCode:4][pri:4]
    //            bit[23]     [22:19]    [18:16]  [15:10]  [9:8]  [7:4]   [3:0]
    // Quadlet 1: [destinationId:16][rCode:4][reserved:12]
    //            bits[31:16]        [15:12]   [11:0]
    // Quadlet 2: [reserved:32] (for responses)
    //
    // The OHCI controller converts this to IEEE 1394 wire format during transmission.
    
    uint32_t header[3]{};
    constexpr uint8_t kSrcBusID = 0;      // Local bus
    constexpr uint8_t kSpeedS400 = 0x02;  // Default S400 speed
    constexpr uint8_t kRetryX = 1;        // Match Linux fw_fill_response() RETRY_1
    constexpr uint8_t kTCodeWriteResponse = 0x2;
    constexpr uint8_t kPriority = 0;

    // Quadlet 0: OHCI AT format (same as PacketBuilder uses)
    header[0] = (static_cast<uint32_t>(kSrcBusID & 0x01) << 23) |  // bit[23]: srcBusID
                (static_cast<uint32_t>(kSpeedS400 & 0x07) << 16) | // bits[18:16]: speed
                (static_cast<uint32_t>(tLabel) << 10) |            // bits[15:10]: tLabel
                (static_cast<uint32_t>(kRetryX) << 8) |            // bits[9:8]: retry
                (static_cast<uint32_t>(kTCodeWriteResponse) << 4) | // bits[7:4]: tCode
                (static_cast<uint32_t>(kPriority) & 0xF);          // bits[3:0]: priority

    // Quadlet 1: destinationId + rCode (for responses)
    header[1] = (static_cast<uint32_t>(destID) << 16) |            // bits[31:16]: destID
                (static_cast<uint32_t>(static_cast<uint8_t>(rcode)) << 12); // bits[15:12]: rCode

    // Quadlet 2: reserved for responses
    header[2] = 0;

    auto chain = builder_.BuildTransactionChain(
        header,
        sizeof(header),
        /*payloadDeviceAddress*/ 0,
        /*payloadSize*/ 0,
        /*needsFlush*/ false);
    if (chain.Empty()) {
        ASFW_LOG_ERROR(Async, "ResponseSender: failed to build WrResp descriptor chain");
        return;
    }

    const auto submitRes = submitter_.submit_tx_chain(atRspCtx, std::move(chain));
    if (submitRes.kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Async, "ResponseSender: submit_tx_chain failed for WrResp (kr=0x%x)", submitRes.kr);
        return;
    }

    ASFW_LOG_V2(Async, "ResponseSender: WrResp queued (tLabel=%u src=0x%04x dst=0x%04x rcode=0x%x)",
                tLabel, srcID, destID, static_cast<unsigned>(rcode));
}

} // namespace ASFW::Async
