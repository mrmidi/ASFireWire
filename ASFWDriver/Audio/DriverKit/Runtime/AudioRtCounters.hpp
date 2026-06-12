#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct AudioRtCounters final {
    std::atomic<uint64_t> ioBeginReadCount{0};
    std::atomic<uint64_t> ioWriteEndCount{0};

    std::atomic<uint64_t> txPackets{0};
    std::atomic<uint64_t> txDataPackets{0};
    std::atomic<uint64_t> txNoDataPackets{0};
    std::atomic<uint64_t> txSilenceSubstitutions{0};
    std::atomic<uint64_t> txUnderruns{0};

    // Refined Phase A counters
    std::atomic<uint64_t> txValidPhasePcmPackets{0};
    std::atomic<uint64_t> txValidPhaseSilencePackets{0};
    std::atomic<uint64_t> txNoPhaseSilencePackets{0};
    std::atomic<uint64_t> txUnderrunSilencePackets{0};
    std::atomic<uint64_t> txStaleSyncPackets{0};
    std::atomic<uint64_t> txInvalidGeometryPackets{0};
    std::atomic<uint64_t> txSytFfffPackets{0};
    std::atomic<uint64_t> txValidSytPackets{0};
    std::atomic<uint64_t> txPcmFramesEncoded{0};
    std::atomic<uint64_t> txPhaseRebases{0};
    std::atomic<uint64_t> txSilenceFallback{0};
    std::atomic<uint64_t> txStaleOverwrittenReads{0};
    std::atomic<uint64_t> txProducerAheadUnderruns{0};
    std::atomic<uint64_t> txPcmNonzeroPackets{0};
    std::atomic<uint64_t> txPcmAllZeroPackets{0};
    std::atomic<uint64_t> txPreparedPcmSlots{0};
    std::atomic<uint64_t> txStartupSilenceSlots{0};
    std::atomic<uint64_t> txReadAheadFaults{0};
    std::atomic<uint64_t> txSourceOverwrittenFaults{0};
    std::atomic<uint64_t> txPreparationDeadlineFaults{0};
    std::atomic<uint64_t> txSlotOwnershipFaults{0};
    std::atomic<uint64_t> txImmediateStops{0};
    std::atomic<uint64_t> txPreparationWakeRequests{0};
    std::atomic<uint64_t> txPreparationWakeDispatches{0};
    std::atomic<uint64_t> txPreparationWakeCoalesced{0};
    std::atomic<uint64_t> txPreparationDrainPasses{0};
    std::atomic<uint64_t> txCompletedPayloadHashMatches{0};
    std::atomic<uint64_t> txCompletedPayloadHashMismatches{0};
    std::atomic<uint64_t> txCompletedPcmSlots{0};
    std::atomic<uint64_t> txCompletedStartupSilenceSlots{0};
    std::atomic<uint64_t> txPayloadMismatchFaults{0};
    std::atomic<uint64_t> txPostLockNoDataPackets{0};

    std::atomic<uint64_t> rxPackets{0};
    std::atomic<uint64_t> rxDecodedFrames{0};
    std::atomic<uint64_t> rxDiscontinuities{0};

    std::atomic<uint64_t> ztsPublished{0};
    std::atomic<uint64_t> ztsRxPublished{0};
    std::atomic<uint64_t> ztsRxAdkPublished{0};

    void Reset() noexcept {
        ioBeginReadCount.store(0, std::memory_order_relaxed);
        ioWriteEndCount.store(0, std::memory_order_relaxed);

        txPackets.store(0, std::memory_order_relaxed);
        txDataPackets.store(0, std::memory_order_relaxed);
        txNoDataPackets.store(0, std::memory_order_relaxed);
        txSilenceSubstitutions.store(0, std::memory_order_relaxed);
        txUnderruns.store(0, std::memory_order_relaxed);

        txValidPhasePcmPackets.store(0, std::memory_order_relaxed);
        txValidPhaseSilencePackets.store(0, std::memory_order_relaxed);
        txNoPhaseSilencePackets.store(0, std::memory_order_relaxed);
        txUnderrunSilencePackets.store(0, std::memory_order_relaxed);
        txStaleSyncPackets.store(0, std::memory_order_relaxed);
        txInvalidGeometryPackets.store(0, std::memory_order_relaxed);
        txSytFfffPackets.store(0, std::memory_order_relaxed);
        txValidSytPackets.store(0, std::memory_order_relaxed);
        txPcmFramesEncoded.store(0, std::memory_order_relaxed);
        txPhaseRebases.store(0, std::memory_order_relaxed);
        txSilenceFallback.store(0, std::memory_order_relaxed);
        txStaleOverwrittenReads.store(0, std::memory_order_relaxed);
        txProducerAheadUnderruns.store(0, std::memory_order_relaxed);
        txPcmNonzeroPackets.store(0, std::memory_order_relaxed);
        txPcmAllZeroPackets.store(0, std::memory_order_relaxed);
        txPreparedPcmSlots.store(0, std::memory_order_relaxed);
        txStartupSilenceSlots.store(0, std::memory_order_relaxed);
        txReadAheadFaults.store(0, std::memory_order_relaxed);
        txSourceOverwrittenFaults.store(0, std::memory_order_relaxed);
        txPreparationDeadlineFaults.store(0, std::memory_order_relaxed);
        txSlotOwnershipFaults.store(0, std::memory_order_relaxed);
        txImmediateStops.store(0, std::memory_order_relaxed);
        txPreparationWakeRequests.store(0, std::memory_order_relaxed);
        txPreparationWakeDispatches.store(0, std::memory_order_relaxed);
        txPreparationWakeCoalesced.store(0, std::memory_order_relaxed);
        txPreparationDrainPasses.store(0, std::memory_order_relaxed);
        txCompletedPayloadHashMatches.store(0, std::memory_order_relaxed);
        txCompletedPayloadHashMismatches.store(0, std::memory_order_relaxed);
        txCompletedPcmSlots.store(0, std::memory_order_relaxed);
        txCompletedStartupSilenceSlots.store(0, std::memory_order_relaxed);
        txPayloadMismatchFaults.store(0, std::memory_order_relaxed);
        txPostLockNoDataPackets.store(0, std::memory_order_relaxed);

        rxPackets.store(0, std::memory_order_relaxed);
        rxDecodedFrames.store(0, std::memory_order_relaxed);
        rxDiscontinuities.store(0, std::memory_order_relaxed);

        ztsPublished.store(0, std::memory_order_relaxed);
        ztsRxPublished.store(0, std::memory_order_relaxed);
        ztsRxAdkPublished.store(0, std::memory_order_relaxed);
    }

    void CountBeginRead() noexcept {
        ioBeginReadCount.fetch_add(1, std::memory_order_relaxed);
    }

    void CountWriteEnd() noexcept {
        ioWriteEndCount.fetch_add(1, std::memory_order_relaxed);
    }

    void CountZtsPublished() noexcept {
        ztsPublished.fetch_add(1, std::memory_order_relaxed);
    }

    void CountRxZtsPublished() noexcept {
        ztsPublished.fetch_add(1, std::memory_order_relaxed);
        ztsRxPublished.fetch_add(1, std::memory_order_relaxed);
    }

    void CountRxAdkZtsPublished() noexcept {
        ztsRxAdkPublished.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace ASFW::Audio::Runtime
