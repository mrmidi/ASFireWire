//
// FCPResponseRouter.hpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP Response Router - routes incoming FCP responses to correct FCPTransport
// Integrates with PacketRouter for block write request handling
//

#pragma once

#include "../Ports/FireWireBusPort.hpp"
#include "../Ports/FireWireRxPort.hpp"
#include "AVCDiscovery.hpp"
#include <vector>

namespace ASFW::Protocols::AVC {

//==============================================================================
// FCP Response Router
//==============================================================================

class FCPResponseRouter {
  public:
    explicit FCPResponseRouter(AVCDiscovery& avcDiscovery,
                               Protocols::Ports::FireWireBusInfo& busInfo)
        : avcDiscovery_(avcDiscovery), busInfo_(busInfo) {}

    Protocols::Ports::BlockWriteDisposition
    RouteBlockWrite(const Protocols::Ports::BlockWriteRequestView& request) {
        ASFW_LOG_V3(FCP,
                    "🔍 FCPResponseRouter::RouteBlockWrite CALLED: srcID=0x%04x payloadLen=%zu",
                    request.sourceID, request.payload.size());

        const uint64_t destOffset = request.destOffset;

        ASFW_LOG_V3(FCP, "🔍 FCPResponseRouter: destOffset=0x%012llx (FCP_RESPONSE=0x%012llx)",
                    destOffset, kFCPResponseAddress);

        if (destOffset != kFCPResponseAddress) {
            ASFW_LOG_V3(FCP, "⚠️  FCPResponseRouter: Not an FCP response (offset mismatch)");
            return Protocols::Ports::BlockWriteDisposition::kAddressError;
        }

        const uint16_t srcNodeID = request.sourceID;
        const uint32_t generation = busInfo_.GetGeneration().value;

        ASFW_LOG_V2(FCP, "✅ FCPResponseRouter: FCP response detected! srcNode=0x%04x gen=%u",
                    srcNodeID, generation);

        FCPTransport* transport = avcDiscovery_.GetFCPTransportForNodeID(srcNodeID);
        if (!transport) {
            ASFW_LOG_V1(FCP, "FCPResponseRouter: FCP response from unknown node 0x%04x", srcNodeID);
            return Protocols::Ports::BlockWriteDisposition::kComplete;
        }

        std::vector<uint8_t> payloadCopy(request.payload.begin(), request.payload.end());

        ASFW_LOG_V2(FCP, "🔄 FCPResponseRouter: Routing to FCPTransport %p (%zu bytes copied)",
                    transport, payloadCopy.size());
        transport->OnFCPResponse(srcNodeID, generation,
                                 std::span<const uint8_t>(payloadCopy.data(), payloadCopy.size()));

        return Protocols::Ports::BlockWriteDisposition::kComplete;
    }

  private:
    AVCDiscovery& avcDiscovery_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
};

} // namespace ASFW::Protocols::AVC
