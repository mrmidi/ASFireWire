#include "LockCommand.hpp"
#include "../Track/Tracking.hpp"
#include "../Tx/PacketBuilder.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include <DriverKit/IOMemoryDescriptor.h>

namespace ASFW::Async {

TxMetadata LockCommand::BuildMetadata(const TransactionContext& txCtx) {
    TxMetadata meta{};
    meta.generation = txCtx.generation;
    meta.sourceNodeID = txCtx.sourceNodeID;
    meta.destinationNodeID = params_.destinationID;
    meta.tCode = 0x9;  // LOCK_REQUEST
    // Lock operations must wait for AR response to know the outcome (rCode + data)
    meta.completionStrategy = CompletionStrategy::CompleteOnAR;

    const uint32_t operandLength = params_.operandLength;
    const uint32_t responseHint = params_.responseLength;

    if (operandLength == 0) {
        meta.expectedLength = 0;
        return meta;
    }

    // For COMPARE_SWAP (extTCode 0x2) with 8-byte operand (compare+swap quadlets),
    // the response payload is the old quadlet (4 bytes). Otherwise fall back to
    // caller-specified responseLength or operandLength.
    if (responseHint != 0) {
        meta.expectedLength = responseHint;
    } else if (extendedTCode_ == 0x2 && operandLength == 8) {
        meta.expectedLength = 4;
    } else {
        meta.expectedLength = operandLength;
    }
    // callback filled by AsyncCommand::Submit()

    return meta;
}

size_t LockCommand::BuildHeader(uint8_t label,
                                const PacketContext& pktCtx,
                                PacketBuilder& builder,
                                uint8_t* buffer) {
    // Delegate to shared PacketBuilder for IEEE 1394 header construction
    return builder.BuildLock(params_, label, extendedTCode_, pktCtx, buffer, 20);
}

std::unique_ptr<PayloadContext> LockCommand::PreparePayload(
    ASFW::Driver::HardwareInterface& hw) {
    
    if (params_.operandLength == 0 || params_.operand == nullptr) {
        return nullptr;
    }
    
    // Lock operand: allocate DMA buffer for compare-and-swap data
    constexpr uint64_t kLockPayloadDirection = kIOMemoryDirectionInOut;  // host writes, controller reads
    return PayloadContext::Create(hw, params_.operand, params_.operandLength, kLockPayloadDirection);
}

} // namespace ASFW::Async
