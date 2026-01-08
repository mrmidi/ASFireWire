#pragma once

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

private:
    DescriptorBuilder& builder_;
    Tx::Submitter& submitter_;
    Engine::ContextManager& ctxMgr_;
    Bus::GenerationTracker& generationTracker_;
};

} // namespace ASFW::Async
