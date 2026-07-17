//
// FCPResponseRouter.hpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP Response Router - routes incoming FCP responses to correct FCPTransport
// Integrates with PacketRouter for block write request handling
//

#pragma once

#include "../Ports/FireWireRxPort.hpp"
#include "../../Logging/Logging.hpp"
#include "FCPTransport.hpp"
#include "IAVCDiscovery.hpp"
#include <vector>

namespace ASFW::Protocols::AVC {

//==============================================================================
// FCP Response Router
//==============================================================================

class FCPResponseRouter {
  public:
    explicit FCPResponseRouter(IAVCDiscovery& avcDiscovery)
        : avcDiscovery_(avcDiscovery) {}

    Protocols::Ports::BlockWriteDisposition
    RouteBlockWrite(const Protocols::Ports::BlockWriteRequestView& request) {
        ASFW_LOG_V3(FCP,
                    "🔍 FCPResponseRouter::RouteBlockWrite CALLED: srcID=0x%04x payloadLen=%zu",
                    request.sourceID, request.payload.size());

        const uint64_t destOffset = request.destOffset;

        ASFW_LOG_V3(FCP, "🔍 FCPResponseRouter: destOffset=0x%012llx (FCP response range=0x%012llx..0x%012llx)",
                    destOffset, kFCPResponseAddress, kFCPResponseAddressEnd - 1);

        if (destOffset < kFCPResponseAddress || destOffset >= kFCPResponseAddressEnd) {
            ASFW_LOG_V3(FCP, "⚠️  FCPResponseRouter: Not an FCP response (offset outside response space)");
            return Protocols::Ports::BlockWriteDisposition::kAddressError;
        }

        if (!request.generation.has_value()) {
            // A response without its receive epoch cannot be associated with
            // an outstanding transaction safely after a bus reset.  Acknowledge
            // the local write, but do not let it complete an AV/C command.
            ASFW_LOG_V1(FCP, "FCPResponseRouter: Dropping response without receive generation");
            return Protocols::Ports::BlockWriteDisposition::kComplete;
        }

        const uint16_t srcNodeID = request.sourceID;
        const uint32_t generation = *request.generation;

        ASFW_LOG_V2(FCP, "✅ FCPResponseRouter: FCP response detected! srcNode=0x%04x gen=%u",
                    srcNodeID, generation);

        // Keep ownership across response delivery. Discovery can erase/rebuild
        // its node map concurrently with a hot-unplug or bus reset.
        auto transport = avcDiscovery_.AcquireFCPTransportForNodeID(srcNodeID);
        if (!transport) {
            ASFW_LOG_V1(FCP, "FCPResponseRouter: FCP response from unknown node 0x%04x", srcNodeID);
            return Protocols::Ports::BlockWriteDisposition::kComplete;
        }

        std::vector<uint8_t> payloadCopy(request.payload.begin(), request.payload.end());

        ASFW_LOG_V2(FCP, "🔄 FCPResponseRouter: Routing to FCPTransport %p (%zu bytes copied)",
                    transport.get(), payloadCopy.size());
        transport->OnFCPResponse(srcNodeID, generation,
                                 std::span<const uint8_t>(payloadCopy.data(), payloadCopy.size()));

        return Protocols::Ports::BlockWriteDisposition::kComplete;
    }

  private:
    IAVCDiscovery& avcDiscovery_;
};

} // namespace ASFW::Protocols::AVC
