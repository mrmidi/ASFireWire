#pragma once

#include "AsyncCommand.hpp"
#include "../AsyncTypes.hpp"

namespace ASFW::Async {

/**
 * PhyCommand - PHY configuration packet (link-local, not IEEE 1394 async).
 * 
 * tCode: 0xE (PHY_PACKET)
 * 
 * Packet format (OHCI §7.8.1.4):
 *   8 bytes header: quadlet1[32], quadlet2[32]
 *   
 * PHY packets are sent locally on the bus (no remote node destination).
 * Used for gap_count configuration, port power management, etc.
 * 
 * No DMA payload - immediate data only.
 */
class PhyCommand : public AsyncCommand<PhyCommand> {
public:
    // Phase 2.3: Removed void* context (captured in callback lambda)
    PhyCommand(PhyParams params, CompletionCallback callback)
        : AsyncCommand(std::move(callback)), params_(params) {}
    
    // CRTP interface implementation
    TxMetadata BuildMetadata(const TransactionContext& txCtx);
    size_t BuildHeader(uint8_t label,
                       const PacketContext& pktCtx,
                       PacketBuilder& builder,
                       uint8_t* buffer);
    std::unique_ptr<PayloadContext> PreparePayload(ASFW::Driver::HardwareInterface&) {
        return nullptr;  // PHY packets use immediate data only
    }

private:
    PhyParams params_;
};

} // namespace ASFW::Async
