//
// FCPResponseRouter.hpp
// ASFWDriver - AV/C Protocol Layer
//
// FCP Response Router - routes incoming FCP responses to correct FCPTransport
// Integrates with PacketRouter for block write request handling
//

#pragma once

#include "AVCDiscovery.hpp"
#include "../../Async/Rx/PacketRouter.hpp"
#include "../../Async/PacketHelpers.hpp"
#include "../../Async/ResponseCode.hpp"
#include "../../Bus/GenerationTracker.hpp"
#include <vector>

namespace ASFW::Protocols::AVC {

using ::ASFW::Async::ResponseCode;

//==============================================================================
// FCP Response Router
//==============================================================================

class FCPResponseRouter {
public:
    explicit FCPResponseRouter(AVCDiscovery& avcDiscovery,
                               Async::Bus::GenerationTracker& generationTracker)
        : avcDiscovery_(avcDiscovery), generationTracker_(generationTracker) {}

    ResponseCode RouteBlockWrite(const Async::ARPacketView& packet) {
        ASFW_LOG_V3(FCP, "🔍 FCPResponseRouter::RouteBlockWrite CALLED: srcID=0x%04x destID=0x%04x payloadLen=%zu",
                 packet.sourceID, packet.destID, packet.payload.size());
        
        uint64_t destOffset = Async::ExtractDestOffset(packet.header);
        
        ASFW_LOG_V3(FCP, "🔍 FCPResponseRouter: destOffset=0x%012llx (FCP_RESPONSE=0x%012llx)",
                 destOffset, kFCPResponseAddress);

        if (destOffset != kFCPResponseAddress) {
            ASFW_LOG_V3(FCP, "⚠️  FCPResponseRouter: Not an FCP response (offset mismatch)");
            return ResponseCode::AddressError;
        }

        uint16_t srcNodeID = packet.sourceID;
        uint32_t generation = generationTracker_.GetCurrentState().generation16;
        
        ASFW_LOG_V2(FCP, "✅ FCPResponseRouter: FCP response detected! srcNode=0x%04x gen=%u",
                 srcNodeID, generation);

        FCPTransport* transport = avcDiscovery_.GetFCPTransportForNodeID(srcNodeID);
        if (!transport) {
            ASFW_LOG_V1(FCP, "FCPResponseRouter: FCP response from unknown node 0x%04x",
                         srcNodeID);
            return ResponseCode::Complete;
        }

        std::vector<uint8_t> payloadCopy(packet.payload.begin(), packet.payload.end());

        ASFW_LOG_V2(FCP, "🔄 FCPResponseRouter: Routing to FCPTransport %p (%zu bytes copied)",
                 transport, payloadCopy.size());
        transport->OnFCPResponse(srcNodeID, generation, 
                                 std::span<const uint8_t>(payloadCopy.data(), payloadCopy.size()));

        return ResponseCode::Complete;
    }

private:
    AVCDiscovery& avcDiscovery_;
    Async::Bus::GenerationTracker& generationTracker_;
};

} // namespace ASFW::Protocols::AVC
