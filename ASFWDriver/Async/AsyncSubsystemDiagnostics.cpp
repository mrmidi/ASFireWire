#include "AsyncSubsystem.hpp"

#include "../Hardware/OHCIDescriptors.hpp"

namespace ASFW::Async {

std::optional<AsyncStatusSnapshot> AsyncSubsystem::GetStatusSnapshot() const {
    if (!contextManager_) {
        return std::nullopt;
    }
    AsyncStatusSnapshot snapshot{};
    if (contextManager_) {
        auto* dm = contextManager_->DmaManager();
        if (dm) {
            snapshot.dmaSlabVirt = reinterpret_cast<uint64_t>(dm->BaseVirtual());
            snapshot.dmaSlabIOVA = dm->BaseIOVA();
            snapshot.dmaSlabSize = static_cast<uint32_t>(dm->TotalSize());
        }
    }

    auto populateDescriptor = [](AsyncDescriptorStatus& out, const DescriptorRing* ring,
                                 const uint8_t* virt, uint64_t iova, uint32_t commandPtr,
                                 uint32_t count, uint32_t stride, uint32_t strideFallback) {
        out.descriptorVirt = reinterpret_cast<uint64_t>(virt);
        out.descriptorIOVA = iova;
        if (count == 0 && ring != nullptr) {
            count = static_cast<uint32_t>(ring->Capacity() + 1); // include sentinel slot
        }
        out.descriptorCount = count;
        out.descriptorStride = stride != 0 ? stride : strideFallback;
        out.commandPtr = commandPtr;
    };

    auto populateBuffers = [](AsyncBufferStatus& out, const BufferRing* ring, const uint8_t* virt,
                              uint64_t iova) {
        out.bufferVirt = reinterpret_cast<uint64_t>(virt);
        out.bufferIOVA = iova;
        if (ring) {
            out.bufferCount = static_cast<uint32_t>(ring->BufferCount());
            out.bufferSize = static_cast<uint32_t>(ring->BufferSize());
        }
    };

    // Populate descriptor info from ContextManager when present
    // Populate descriptor info and buffer rings from ContextManager
    {
        auto* atReqRing = contextManager_->AtRequestRing();
        populateDescriptor(snapshot.atRequest, atReqRing, nullptr, 0, 0, 0, 0,
                           static_cast<uint32_t>(sizeof(HW::OHCIDescriptorImmediate)));
    }
    {
        auto* atRspRing = contextManager_->AtResponseRing();
        populateDescriptor(snapshot.atResponse, atRspRing, nullptr, 0, 0, 0, 0,
                           static_cast<uint32_t>(sizeof(HW::OHCIDescriptorImmediate)));
    }
    {
        auto* arReqRing = contextManager_->ArRequestRing();
        populateDescriptor(snapshot.arRequest, nullptr, nullptr, 0, 0, 0, 0,
                           static_cast<uint32_t>(sizeof(HW::OHCIDescriptor)));
        populateBuffers(snapshot.arRequestBuffers, arReqRing, nullptr, 0);
    }
    {
        auto* arRspRing = contextManager_->ArResponseRing();
        populateDescriptor(snapshot.arResponse, nullptr, nullptr, 0, 0, 0, 0,
                           static_cast<uint32_t>(sizeof(HW::OHCIDescriptor)));
        populateBuffers(snapshot.arResponseBuffers, arRspRing, nullptr, 0);
    }

    return snapshot;
}

DMAMemoryManager* AsyncSubsystem::GetDMAManager() {
    return contextManager_ ? contextManager_->DmaManager() : nullptr;
}

AsyncWatchdogStats AsyncSubsystem::GetWatchdogStats() const {
    AsyncWatchdogStats stats{};
    stats.tickCount = watchdogTickCount_.load(std::memory_order_relaxed);
    stats.expiredTransactions = watchdogExpiredCount_.load(std::memory_order_relaxed);
    stats.drainedTxCompletions = watchdogDrainedCompletions_.load(std::memory_order_relaxed);
    stats.contextsRearmed = watchdogContextsRearmed_.load(std::memory_order_relaxed);
    stats.lastTickUsec = watchdogLastTickUsec_.load(std::memory_order_relaxed);
    return stats;
}

void AsyncSubsystem::DumpState() {
    // TODO: emit structured diagnostics for debugging.
}

} // namespace ASFW::Async
