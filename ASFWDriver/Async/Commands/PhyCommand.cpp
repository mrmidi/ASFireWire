#include "PhyCommand.hpp"
#include "../Track/Tracking.hpp"
#include "../Tx/PacketBuilder.hpp"

namespace ASFW::Async {

TxMetadata PhyCommand::BuildMetadata(const TransactionContext& txCtx) {
    TxMetadata meta{};
    meta.generation = txCtx.generation;
    meta.sourceNodeID = txCtx.sourceNodeID;
    meta.destinationNodeID = 0xFFFF;  // PHY packets are link-local (no remote destination)
    meta.tCode = 0xE;  // PHY_PACKET
    meta.expectedLength = 0;  // PHY packets don't generate responses
    meta.completionStrategy = CompletionStrategy::CompleteOnPHY;  // PHY packets use dedicated strategy
    // callback filled by AsyncCommand::Submit()

    return meta;
}

size_t PhyCommand::BuildHeader(uint8_t label,
                               const PacketContext& pktCtx,
                               PacketBuilder& builder,
                               uint8_t* buffer) {
    (void)label;
    (void)pktCtx;
    // Delegate to shared PacketBuilder for IEEE 1394 header construction
    return builder.BuildPhyPacket(params_, buffer, 16);
}

} // namespace ASFW::Async
