#pragma once

#include "../ForwardDecls.hpp"
#include "../Track/Tracking.hpp"
#include "../Track/CompletionQueue.hpp"
#include "../../Debug/BusResetPacketCapture.hpp"
#include "../Contexts/ARRequestContext.hpp"
#include "../Contexts/ARResponseContext.hpp"
#include "PacketRouter.hpp"
#include "ARPacketParser.hpp"
#include "../../Hardware/OHCIEventCodes.hpp"
#include "../../Bus/GenerationTracker.hpp"
#include <memory>

namespace ASFW::Async::Rx {

// Define the concrete Tracking type alias for clarity
using TrackingActor = Track_Tracking<CompletionQueue>;

class RxPath {
public:
    // Constructor takes references to all the actors it collaborates with.
    RxPath(ARRequestContext& arReqContext,
           ARResponseContext& arRespContext,
           TrackingActor& tracking,
           ASFW::Async::Bus::GenerationTracker& generationTracker,
           PacketRouter& packetRouter);

    // This single method will be called by the engine on an RX interrupt.
    void ProcessARInterrupts(std::atomic<uint32_t>& is_bus_reset_in_progress,
                             bool isRunning,
                             Debug::BusResetPacketCapture* busResetCapture);

private:
    // Private helper to process a single parsed packet.
    void ProcessReceivedPacket(ARContextType contextType,
                               const ARPacketParser::PacketInfo& info,
                               Debug::BusResetPacketCapture* busResetCapture);

    // Handle synthetic bus reset packet
    void HandleSyntheticBusResetPacket(const uint32_t* quadlets,
                                       uint8_t newGeneration,
                                       Debug::BusResetPacketCapture* busResetCapture);

    // References to collaborators (owned by the engine).
    ARRequestContext& arRequestContext_;
    ARResponseContext& arResponseContext_;
    TrackingActor& tracking_;
    ASFW::Async::Bus::GenerationTracker& generationTracker_;
    PacketRouter& packetRouter_;

    // RxPath owns the parser.
    std::unique_ptr<ARPacketParser> packetParser_;

    // Handle synthetic / general PHY packets coming via PacketRouter
    void HandlePhyRequestPacket(const ARPacketView& view);

    // Current bus-reset capture target for this interrupt pass
    Debug::BusResetPacketCapture* currentBusResetCapture_ = nullptr;
};

} // namespace ASFW::Async::Rx
