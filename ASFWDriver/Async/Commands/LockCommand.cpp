#include "LockCommand.hpp"
#include "../Track/Tracking.hpp"
#include "../Tx/PacketBuilder.hpp"
#include "../../Core/HardwareInterface.hpp"

namespace ASFW::Async {

TxMetadata LockCommand::BuildMetadata(const TransactionContext& txCtx) {
    TxMetadata meta{};
    meta.generation = txCtx.generation;
    meta.sourceNodeID = txCtx.sourceNodeID;
    meta.destinationNodeID = params_.destinationID;
    meta.tCode = 0x9;  // LOCK_REQUEST
    meta.expectedLength = params_.length;  // Expect lock result in response
    // callback filled by AsyncCommand::Submit()
    
    return meta;
}

size_t LockCommand::BuildHeader(uint8_t label, const PacketContext& pktCtx, uint8_t* buffer) {
    // Delegate to PacketBuilder for IEEE 1394 header construction
    PacketBuilder builder;
    return builder.BuildLock(params_, extendedTCode_, label, pktCtx, buffer, 20);
}

std::unique_ptr<PayloadContext> LockCommand::PreparePayload(
    ASFW::Driver::HardwareInterface& hw) {
    
    if (params_.length == 0 || params_.operand == nullptr) {
        return nullptr;
    }
    
    // Lock operand: allocate DMA buffer for compare-and-swap data
    constexpr uint64_t kIOMemoryDirectionOut = 0;  // Host→Device
    return PayloadContext::Create(hw, params_.operand, params_.length, kIOMemoryDirectionOut);
}

} // namespace ASFW::Async
