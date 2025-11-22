#include "WriteCommand.hpp"
#include "../Track/Tracking.hpp"
#include "../Tx/PacketBuilder.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>

namespace ASFW::Async {

TxMetadata WriteCommand::BuildMetadata(const TransactionContext& txCtx) {
    const bool isQuadlet = (params_.length == 4);
    
    TxMetadata meta{};
    meta.generation = txCtx.generation;
    meta.sourceNodeID = txCtx.sourceNodeID;
    meta.destinationNodeID = params_.destinationID;
    meta.tCode = isQuadlet ? 0x0 : 0x1;  // WRITE_QUADLET or WRITE_BLOCK
    meta.expectedLength = 0;  // Writes don't expect response payload
    // callback filled by AsyncCommand::Submit()
    
    return meta;
}

size_t WriteCommand::BuildHeader(uint8_t label,
                                 const PacketContext& pktCtx,
                                 PacketBuilder& builder,
                                 uint8_t* buffer) {
    // Delegate to shared PacketBuilder for IEEE 1394 header construction
    const bool isQuadlet = (params_.length == 4);
    if (isQuadlet) {
        return builder.BuildWriteQuadlet(params_, label, pktCtx, buffer, 20);
    } else {
        return builder.BuildWriteBlock(params_, label, pktCtx, buffer, 20);
    }
}

std::unique_ptr<PayloadContext> WriteCommand::PreparePayload(
    ASFW::Driver::HardwareInterface& hw) {
    
    // Quadlet writes use immediate data (embedded in header), no DMA needed
    const bool isQuadlet = (params_.length == 4);
    if (isQuadlet || params_.length == 0) {
        return nullptr;
    }
    
    // Block write: allocate DMA buffer for payload
    constexpr uint64_t kWritePayloadDirection = kIOMemoryDirectionInOut;  // host writes, controller reads
    return PayloadContext::Create(hw, params_.payload, params_.length, kWritePayloadDirection);
}

} // namespace ASFW::Async
