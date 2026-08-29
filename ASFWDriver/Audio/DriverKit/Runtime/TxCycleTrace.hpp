#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

inline constexpr uint32_t kTxCycleTraceCapacity = 512;

struct TxCycleTraceRecord final {
    uint64_t epoch{0};
    uint64_t cycleOrdinal{0};
    uint64_t firstAudioFrame{0};
    uint64_t presentationBusTicks{0};
    uint64_t prepareCycle{0};
    uint64_t publishCycle{0};
    uint64_t ownershipCycle{0};
    uint64_t completionCycle{0};
    uint32_t frameCount{0};
    uint32_t pcmResult{0};
    uint32_t disposition{0};
    uint32_t deadlineHeadroomCycles{0};
};

struct TxCycleTraceSlot final {
    std::atomic<uint64_t> sequence{0};
    std::atomic<uint64_t> epoch{0};
    std::atomic<uint64_t> cycleOrdinal{0};
    std::atomic<uint64_t> firstAudioFrame{0};
    std::atomic<uint64_t> presentationBusTicks{0};
    std::atomic<uint64_t> prepareCycle{0};
    std::atomic<uint64_t> publishCycle{0};
    std::atomic<uint64_t> ownershipCycle{0};
    std::atomic<uint64_t> completionCycle{0};
    std::atomic<uint32_t> frameCount{0};
    std::atomic<uint32_t> pcmResult{0};
    std::atomic<uint32_t> disposition{0};
    std::atomic<uint32_t> deadlineHeadroomCycles{0};

    void Reset() noexcept {
        sequence.store(0, std::memory_order_relaxed);
        epoch.store(0, std::memory_order_relaxed);
        cycleOrdinal.store(0, std::memory_order_relaxed);
        firstAudioFrame.store(0, std::memory_order_relaxed);
        presentationBusTicks.store(0, std::memory_order_relaxed);
        prepareCycle.store(0, std::memory_order_relaxed);
        publishCycle.store(0, std::memory_order_relaxed);
        ownershipCycle.store(0, std::memory_order_relaxed);
        completionCycle.store(0, std::memory_order_relaxed);
        frameCount.store(0, std::memory_order_relaxed);
        pcmResult.store(0, std::memory_order_relaxed);
        disposition.store(0, std::memory_order_relaxed);
        deadlineHeadroomCycles.store(0, std::memory_order_relaxed);
    }

    void Publish(const TxCycleTraceRecord& record,
                 uint64_t generation) noexcept {
        sequence.store(generation * 2U + 1U, std::memory_order_release);
        epoch.store(record.epoch, std::memory_order_relaxed);
        cycleOrdinal.store(record.cycleOrdinal, std::memory_order_relaxed);
        firstAudioFrame.store(record.firstAudioFrame,
                              std::memory_order_relaxed);
        presentationBusTicks.store(record.presentationBusTicks,
                                   std::memory_order_relaxed);
        prepareCycle.store(record.prepareCycle, std::memory_order_relaxed);
        publishCycle.store(record.publishCycle, std::memory_order_relaxed);
        ownershipCycle.store(record.ownershipCycle,
                             std::memory_order_relaxed);
        completionCycle.store(record.completionCycle,
                              std::memory_order_relaxed);
        frameCount.store(record.frameCount, std::memory_order_relaxed);
        pcmResult.store(record.pcmResult, std::memory_order_relaxed);
        disposition.store(record.disposition, std::memory_order_relaxed);
        deadlineHeadroomCycles.store(record.deadlineHeadroomCycles,
                                     std::memory_order_relaxed);
        sequence.store(generation * 2U + 2U, std::memory_order_release);
    }
};

struct TxCycleTraceRing final {
    std::atomic<uint64_t> writeCount{0};
    std::array<TxCycleTraceSlot, kTxCycleTraceCapacity> records{};

    void Reset() noexcept {
        writeCount.store(0, std::memory_order_relaxed);
        for (auto& record : records) record.Reset();
    }

    void Publish(const TxCycleTraceRecord& record) noexcept {
        const uint64_t generation = writeCount.fetch_add(
            1, std::memory_order_relaxed);
        records[generation % records.size()].Publish(record, generation);
    }

    void Complete(uint64_t epoch,
                  uint64_t cycleOrdinal,
                  uint64_t completionCycle) noexcept {
        const uint64_t count = writeCount.load(std::memory_order_acquire);
        const uint64_t available = count < records.size()
            ? count : records.size();
        for (uint64_t back = 0; back < available; ++back) {
            auto& slot = records[(count - 1U - back) % records.size()];
            const uint64_t before = slot.sequence.load(
                std::memory_order_acquire);
            if ((before & 1U) != 0U ||
                slot.epoch.load(std::memory_order_relaxed) != epoch ||
                slot.cycleOrdinal.load(std::memory_order_relaxed) !=
                    cycleOrdinal) {
                continue;
            }
            slot.sequence.store(before + 1U, std::memory_order_release);
            slot.completionCycle.store(completionCycle,
                                       std::memory_order_relaxed);
            slot.sequence.store(before + 2U, std::memory_order_release);
            return;
        }
    }
};

static_assert(std::atomic<uint64_t>::is_always_lock_free);

} // namespace ASFW::Audio::Runtime
