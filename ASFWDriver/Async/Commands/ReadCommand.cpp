#include "ReadCommand.hpp"
#include "../Track/Tracking.hpp"
#include "../Tx/PacketBuilder.hpp"

namespace ASFW::Async {

TxMetadata ReadCommand::BuildMetadata(const TransactionContext& txCtx) {
    const bool isQuadlet = (params_.length == 0 || params_.length == 4);

    TxMetadata meta{};
    meta.generation = txCtx.generation;
    meta.sourceNodeID = txCtx.sourceNodeID;
    meta.destinationNodeID = params_.destinationID;
    meta.tCode = isQuadlet ? 0x4 : 0x5;  // READ_QUADLET or READ_BLOCK
    meta.expectedLength = params_.length;

    // EXPLICIT: Read operations complete on AR response only (gotPacket model)
    // Per Apple IOFWReadQuadCommand: gotAck() stores ack, gotPacket() completes
    meta.completionStrategy = CompletionStrategy::CompleteOnAR;

    // callback filled by AsyncCommand::Submit()

    return meta;
}

size_t ReadCommand::BuildHeader(uint8_t label, const PacketContext& pktCtx, uint8_t* buffer) {
    // Delegate to PacketBuilder for IEEE 1394 header construction
    PacketBuilder builder;
    
    const bool isQuadlet = (params_.length == 0 || params_.length == 4);
    if (isQuadlet) {
        return builder.BuildReadQuadlet(params_, label, pktCtx, buffer, 16);
    } else {
        return builder.BuildReadBlock(params_, label, pktCtx, buffer, 20);
    }
}

} // namespace ASFW::Async
