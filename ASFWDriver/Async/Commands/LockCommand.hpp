#pragma once

#include "AsyncCommand.hpp"
#include "../AsyncTypes.hpp"

namespace ASFW::Async {

/**
 * LockCommand - IEEE 1394 compare-and-swap lock request.
 * 
 * tCode: 0x9 (LOCK_REQUEST) with extended_tCode specifying operation type
 * 
 * Packet format (OHCI §7.8.1.3):
 *   16 bytes header + operand payload (DMA scatter-gather)
 *   
 * Extended tCodes (IEEE 1394-1995 Table 6-4):
 *   0x1 = MASK_SWAP (compare not used, operand is mask+data)
 *   0x2 = COMPARE_SWAP (operand is compare_value+data)
 *   0x3 = FETCH_ADD (operand is data to add)
 *   0x4 = LITTLE_ADD (operand is data, byte-swap result)
 *   0x5 = BOUNDED_ADD (operand is data, clamp on overflow)
 *   0x6 = WRAP_ADD (operand is data, wrap on overflow)
 * 
 * Operand payload is DMA-mapped and attached to PayloadRegistry.
 */
class LockCommand : public AsyncCommand<LockCommand> {
public:
    // Phase 2.3: Removed void* context (captured in callback lambda)
    LockCommand(LockParams params, uint16_t extendedTCode,
                CompletionCallback callback)
        : AsyncCommand(std::move(callback)), params_(params), extendedTCode_(extendedTCode) {}
    
    // CRTP interface implementation
    TxMetadata BuildMetadata(const TransactionContext& txCtx);
    size_t BuildHeader(uint8_t label,
                       const PacketContext& pktCtx,
                       PacketBuilder& builder,
                       uint8_t* buffer);
    std::unique_ptr<PayloadContext> PreparePayload(ASFW::Driver::HardwareInterface& hw);

private:
    LockParams params_;
    uint16_t extendedTCode_;
};

} // namespace ASFW::Async
