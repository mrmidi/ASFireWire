#include "ASFWDriver/Async/Tx/ResponseSender.hpp"

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
    // Stub implementation
    (void)request;
    (void)rcode;
}

void ResponseSender::SendReadQuadletResponse(const ARPacketView& request,
                                             ResponseCode rcode,
                                             uint32_t quadletData) noexcept {
    (void)request;
    (void)rcode;
    (void)quadletData;
}

void ResponseSender::SendReadBlockResponse(const ARPacketView& request,
                                           ResponseCode rcode,
                                           uint64_t payloadDeviceAddress,
                                           uint32_t payloadLength) noexcept {
    (void)request;
    (void)rcode;
    (void)payloadDeviceAddress;
    (void)payloadLength;
}

void ResponseSender::SendResponse(const ARPacketView& request,
                                  ResponseCode rcode,
                                  uint8_t responseTCode,
                                  const uint32_t* header,
                                  std::size_t headerBytes,
                                  uint64_t payloadDeviceAddress,
                                  std::size_t payloadLength) noexcept {
    (void)request;
    (void)rcode;
    (void)responseTCode;
    (void)header;
    (void)headerBytes;
    (void)payloadDeviceAddress;
    (void)payloadLength;
}

} // namespace ASFW::Async
