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

namespace {

constexpr uint8_t kSrcBusID = 0;
constexpr uint8_t kSpeedS400 = 0x02;
constexpr uint8_t kRetryX = 1;
constexpr uint8_t kPriority = 0;

uint32_t BuildQ0(uint8_t tLabel, uint8_t tCode) {
    return (static_cast<uint32_t>(kSrcBusID & 0x01) << 23) |
           (static_cast<uint32_t>(kSpeedS400 & 0x07) << 16) |
           (static_cast<uint32_t>(tLabel & 0x3F) << 10) |
           (static_cast<uint32_t>(kRetryX) << 8) |
           (static_cast<uint32_t>(tCode & 0x0F) << 4) |
           (static_cast<uint32_t>(kPriority) & 0x0F);
}

uint32_t BuildQ1(uint16_t destID, ResponseCode rcode) {
    return (static_cast<uint32_t>(destID) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(rcode) & 0x0F) << 12);
}

} // namespace

ResponseSender::ResponseSender(DescriptorBuilder& builder,
                               Tx::Submitter& submitter,
                               Engine::ContextManager& ctxMgr,
                               Bus::GenerationTracker& generationTracker) noexcept
    : builder_(builder)
    , submitter_(submitter)
    , ctxMgr_(ctxMgr)
    , generationTracker_(generationTracker) {}

void ResponseSender::SendResponse(const ARPacketView& request,
                                  ResponseCode rcode,
                                  uint8_t responseTCode,
                                  const uint32_t* header,
                                  std::size_t headerBytes,
                                  uint64_t payloadDeviceAddress,
                                  std::size_t payloadLength) noexcept {
    // Per IEEE 1394, broadcast requests (destID=0xFFFF) do not get responses.
    if (request.destID == 0xFFFF) {
        ASFW_LOG_V3(Async, "ResponseSender: skip response for broadcast destID=0xFFFF");
        return;
    }

    auto* atRspCtx = ctxMgr_.GetAtResponseContext();
    if (!atRspCtx) {
        ASFW_LOG_ERROR(Async, "ResponseSender: ATResponseContext unavailable, cannot send response");
        return;
    }

    auto chain = builder_.BuildTransactionChain(
        header,
        headerBytes,
        payloadDeviceAddress,
        payloadLength,
        /*needsFlush*/ false);
    if (chain.Empty()) {
        ASFW_LOG_ERROR(
            Async,
            "ResponseSender: failed to build response chain (tCode=0x%x payload=%zu)",
            responseTCode,
            payloadLength);
        return;
    }

    const auto submitRes = submitter_.submit_tx_chain(atRspCtx, std::move(chain));
    if (submitRes.kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(
            Async,
            "ResponseSender: submit_tx_chain failed (tCode=0x%x kr=0x%x)",
            responseTCode,
            submitRes.kr);
        return;
    }

    ASFW_LOG_V2(
        Async,
        "ResponseSender: response queued (tCode=0x%x tLabel=%u dst=0x%04x rcode=0x%x payload=%zu)",
        responseTCode,
        static_cast<unsigned>(request.tLabel & 0x3F),
        request.sourceID,
        static_cast<unsigned>(rcode),
        payloadLength);
}

void ResponseSender::SendWriteResponse(const ARPacketView& request, ResponseCode rcode) noexcept {
    // Only write requests (quadlet/block) receive a WrResp.
    if (request.tCode != 0x0 && request.tCode != 0x1) {
        ASFW_LOG_V3(Async, "ResponseSender: skip WrResp for non-write tCode=0x%x", request.tCode);
        return;
    }

    uint32_t header[3]{};
    header[0] = BuildQ0(static_cast<uint8_t>(request.tLabel & 0x3F), /*WrResp*/ 0x2);
    header[1] = BuildQ1(request.sourceID, rcode);
    header[2] = 0;

    SendResponse(request,
                 rcode,
                 /*responseTCode*/ 0x2,
                 header,
                 sizeof(header),
                 /*payloadDeviceAddress*/ 0,
                 /*payloadLength*/ 0);
}

void ResponseSender::SendReadQuadletResponse(const ARPacketView& request,
                                             ResponseCode rcode,
                                             uint32_t quadletData) noexcept {
    if (request.tCode != 0x4) {
        ASFW_LOG_V3(Async,
                    "ResponseSender: skip RdQuadResp for non-read-quadlet tCode=0x%x",
                    request.tCode);
        return;
    }

    uint32_t header[4]{};
    header[0] = BuildQ0(static_cast<uint8_t>(request.tLabel & 0x3F), /*RdQuadResp*/ 0x6);
    header[1] = BuildQ1(request.sourceID, rcode);
    header[2] = 0;
    header[3] = (rcode == ResponseCode::Complete) ? quadletData : 0;

    SendResponse(request,
                 rcode,
                 /*responseTCode*/ 0x6,
                 header,
                 sizeof(header),
                 /*payloadDeviceAddress*/ 0,
                 /*payloadLength*/ 0);
}

void ResponseSender::SendReadBlockResponse(const ARPacketView& request,
                                           ResponseCode rcode,
                                           uint64_t payloadDeviceAddress,
                                           uint32_t payloadLength) noexcept {
    if (request.tCode != 0x5) {
        ASFW_LOG_V3(Async,
                    "ResponseSender: skip RdBlockResp for non-read-block tCode=0x%x",
                    request.tCode);
        return;
    }

    uint32_t header[4]{};
    header[0] = BuildQ0(static_cast<uint8_t>(request.tLabel & 0x3F), /*RdBlockResp*/ 0x7);
    header[1] = BuildQ1(request.sourceID, rcode);
    header[2] = 0;

    std::size_t responsePayloadLen = 0;
    uint64_t responsePayloadAddress = 0;

    if (rcode == ResponseCode::Complete && payloadLength > 0) {
        header[3] = static_cast<uint32_t>(payloadLength) << 16;
        responsePayloadLen = payloadLength;
        responsePayloadAddress = payloadDeviceAddress;
    } else {
        header[3] = 0;
    }

    SendResponse(request,
                 rcode,
                 /*responseTCode*/ 0x7,
                 header,
                 sizeof(header),
                 responsePayloadAddress,
                 responsePayloadLen);
}

} // namespace ASFW::Async
