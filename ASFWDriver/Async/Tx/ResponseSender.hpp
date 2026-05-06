#pragma once

#include <cstddef>
#include <cstdint>

#include "../ResponseCode.hpp"
#include "../Rx/PacketRouter.hpp"

namespace ASFW::Async {

class DescriptorBuilder;
class ATResponseContext;
class IFireWireBusInfo;
namespace Bus { class GenerationTracker; }
namespace Engine { class ContextManager; }
namespace Tx { class Submitter; }

/// Utility to build and send Write Response (WrResp) packets for incoming AR requests.
class ResponseSender {
public:
    ResponseSender(DescriptorBuilder& builder,
                   Tx::Submitter& submitter,
                   Engine::ContextManager& ctxMgr,
                   Bus::GenerationTracker& generationTracker) noexcept;

    /// Build and transmit a WrResp for the given request packet.
    /// Skips transmission for broadcast requests (destID=0xFFFF).
    void SendWriteResponse(const ARPacketView& request, ResponseCode rcode) noexcept;

    /// Build and transmit a Read Quadlet Response (tCode 0x6).
    void SendReadQuadletResponse(const ARPacketView& request,
                                 ResponseCode rcode,
                                 uint32_t quadletData) noexcept;

    /// Build and transmit a Read Block Response (tCode 0x7).
    void SendReadBlockResponse(const ARPacketView& request,
                               ResponseCode rcode,
                               uint64_t payloadDeviceAddress,
                               uint32_t payloadLength) noexcept;

private:
    void SendResponse(const ARPacketView& request,
                      ResponseCode rcode,
                      uint8_t responseTCode,
                      const uint32_t* header,
                      std::size_t headerBytes,
                      uint64_t payloadDeviceAddress,
                      std::size_t payloadLength) noexcept;

    DescriptorBuilder& builder_;
    Tx::Submitter& submitter_;
    Engine::ContextManager& ctxMgr_;
    Bus::GenerationTracker& generationTracker_;
};

} // namespace ASFW::Async
