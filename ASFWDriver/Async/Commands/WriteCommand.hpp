#pragma once

#include "AsyncCommand.hpp"
#include "../AsyncTypes.hpp"

namespace ASFW::Async {

/**
 * WriteCommand - IEEE 1394 block/quadlet write request.
 * 
 * tCode: 0x0 (WRITE_QUADLET_REQUEST) if length==4
 *        0x1 (WRITE_BLOCK_REQUEST) otherwise
 * 
 * Packet format (OHCI §7.8.1.2):
 *   Quadlet write: 16 bytes header + 4 bytes immediate data
 *   Block write:   16 bytes header + N bytes payload (DMA scatter-gather)
 * 
 * Payload is DMA-mapped via PayloadContext and attached to PayloadRegistry
 * for lifetime management across async completion.
 */
class WriteCommand : public AsyncCommand<WriteCommand> {
public:
    // Phase 2.3: Removed void* context (captured in callback lambda)
    WriteCommand(WriteParams params, CompletionCallback callback)
        : AsyncCommand(std::move(callback)), params_(params) {}
    
    // CRTP interface implementation
    TxMetadata BuildMetadata(const TransactionContext& txCtx);
    size_t BuildHeader(uint8_t label, const PacketContext& pktCtx, uint8_t* buffer);
    std::unique_ptr<PayloadContext> PreparePayload(ASFW::Driver::HardwareInterface& hw);

private:
    WriteParams params_;
};

} // namespace ASFW::Async
