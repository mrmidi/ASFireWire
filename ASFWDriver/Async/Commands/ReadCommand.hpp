#pragma once

#include "AsyncCommand.hpp"
#include "../AsyncTypes.hpp"

namespace ASFW::Async {

/**
 * ReadCommand - IEEE 1394 block/quadlet read request.
 * 
 * tCode: 0x4 (READ_QUADLET_REQUEST) if length==0 or length==4
 *        0x5 (READ_BLOCK_REQUEST) otherwise
 * 
 * Packet format (OHCI §7.8.1.1):
 *   Quadlet read:  12 bytes header (destination[16], tLabel[6], rt[2], tCode[4], 
 *                                   source[16], destination_offset[48])
 *   Block read:    16 bytes header (adds data_length[16], extended_tCode[16])
 * 
 * No payload transmission (read fetches data from remote node).
 */
class ReadCommand : public AsyncCommand<ReadCommand> {
public:
    // Phase 2.3: Removed void* context (captured in callback lambda)
    ReadCommand(ReadParams params, CompletionCallback callback)
        : AsyncCommand(std::move(callback)), params_(params) {}
    
    // CRTP interface implementation
    TxMetadata BuildMetadata(const TransactionContext& txCtx);
    size_t BuildHeader(uint8_t label,
                       const PacketContext& pktCtx,
                       PacketBuilder& builder,
                       uint8_t* buffer);
    std::unique_ptr<PayloadContext> PreparePayload(ASFW::Driver::HardwareInterface&) {
        return nullptr;  // Reads don't send payload
    }

private:
    ReadParams params_;
};

} // namespace ASFW::Async
