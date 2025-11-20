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
#include "../../Bus/GenerationTracker.hpp"

namespace ASFW::Protocols::AVC {

//==============================================================================
// FCP Response Router
//==============================================================================

/// FCP Response Router
///
/// Routes FCP response packets (block writes to FCP_RESPONSE_ADDRESS)
/// to the correct FCPTransport instance based on source node ID.
///
/// **Integration**:
/// ```cpp
/// // Register handler with PacketRouter
/// packetRouter.RegisterRequestHandler(0x1, [this](const ARPacketView& pkt) {
///     fcpResponseRouter->RouteBlockWrite(pkt);
/// });
/// ```
///
/// **Packet Flow**:
/// 1. Device sends block write to our FCP_RESPONSE_ADDRESS (0xFFFFF0000D00)
/// 2. OHCI receives in AR Request context (tCode 0x1)
/// 3. PacketRouter dispatches to our block write handler
/// 4. FCPResponseRouter extracts destOffset, checks if FCP response
/// 5. If FCP, looks up FCPTransport by source nodeID
/// 6. Calls FCPTransport::OnFCPResponse() with payload
class FCPResponseRouter {
public:
    /// Constructor
    ///
    /// @param avcDiscovery AV/C discovery instance (maintains nodeID → transport map)
    /// @param generationTracker Generation tracker for current generation
    explicit FCPResponseRouter(AVCDiscovery& avcDiscovery,
                               Async::Bus::GenerationTracker& generationTracker)
        : avcDiscovery_(avcDiscovery), generationTracker_(generationTracker) {}

    /// Route block write request
    ///
    /// Called by PacketRouter for all block write requests (tCode 0x1).
    /// Detects FCP responses and routes to appropriate FCPTransport.
    ///
    /// @param packet Parsed packet view
    /// @return true if packet was FCP response (handled), false otherwise
    bool RouteBlockWrite(const Async::ARPacketView& packet) {
        // Extract destination offset from header
        uint64_t destOffset = Async::ExtractDestOffset(packet.header);

        // Check if this is an FCP response
        if (destOffset != kFCPResponseAddress) {
            return false;  // Not FCP, caller should handle
        }

        // This is an FCP response - route to correct transport
        uint16_t srcNodeID = packet.sourceID;
        uint32_t generation = generationTracker_.GetCurrentState().generation16;

        FCPTransport* transport = avcDiscovery_.GetFCPTransportForNodeID(srcNodeID);
        if (!transport) {
            os_log_error(OS_LOG_DEFAULT,
                         "FCPResponseRouter: FCP response from unknown node %u",
                         srcNodeID);
            return true;  // Still FCP, but no handler
        }

        // Route to transport
        transport->OnFCPResponse(srcNodeID, generation, packet.payload);

        return true;  // Handled
    }

private:
    AVCDiscovery& avcDiscovery_;
    Async::Bus::GenerationTracker& generationTracker_;
};

} // namespace ASFW::Protocols::AVC
