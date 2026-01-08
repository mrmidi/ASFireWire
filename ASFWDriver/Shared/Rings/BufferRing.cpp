#include "BufferRing.hpp"

#include <atomic>
#include <cstring>

#include "../../Common/BarrierUtils.hpp"
#include "../Memory/DMAMemoryManager.hpp"
#include "../Memory/IDMAMemory.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"

namespace ASFW::Shared {

bool BufferRing::Initialize(std::span<HW::OHCIDescriptor> descriptors, std::span<uint8_t> buffers, size_t bufferCount, size_t bufferSize) noexcept {
    if (descriptors.empty() || buffers.empty()) {
        ASFW_LOG(Async, "BufferRing::Initialize: empty storage");
        return false;
    }
    if (descriptors.size() != bufferCount) {
        ASFW_LOG(Async, "BufferRing::Initialize: descriptor count %zu != buffer count %zu", descriptors.size(), bufferCount);
        return false;
    }
    if (buffers.size() < bufferCount * bufferSize) {
        ASFW_LOG(Async, "BufferRing::Initialize: buffer storage too small (%zu < %zu)", buffers.size(), bufferCount * bufferSize);
        return false;
    }
    if (reinterpret_cast<uintptr_t>(descriptors.data()) % 16 != 0) {
        ASFW_LOG(Async, "BufferRing::Initialize: descriptors not 16-byte aligned");
        return false;
    }
    descriptors_ = descriptors;
    buffers_ = buffers;
    bufferCount_ = bufferCount;
    bufferSize_ = bufferSize;
    head_ = 0;
    for (size_t i = 0; i < bufferCount; ++i) {
        auto& desc = descriptors_[i];
        std::memset(&desc, 0, sizeof(desc));
        constexpr uint32_t kCmdInputMore = HW::OHCIDescriptor::kCmdInputMore;
        constexpr uint32_t kKeyStandard = HW::OHCIDescriptor::kKeyStandard;
        constexpr uint32_t kS = 1u;  
        constexpr uint32_t kIntAlways = HW::OHCIDescriptor::kIntAlways;
        constexpr uint32_t kBranchAlways = HW::OHCIDescriptor::kBranchAlways;
        desc.control = (kCmdInputMore << 28) | (kKeyStandard << 25) | (kS << 24) | (kIntAlways << 22) | (kBranchAlways << 20) | static_cast<uint32_t>(bufferSize);
        desc.dataAddress = static_cast<uint32_t>(i * bufferSize);
        size_t nextIndex = (i + 1) % bufferCount;
        desc.branchWord = (1u << 28) | (static_cast<uint32_t>(nextIndex) << 4);
        HW::AR_init_status(desc, static_cast<uint16_t>(bufferSize));
    }
    ASFW_LOG(Async, "BufferRing initialized: %zu buffers x %zu bytes", bufferCount, bufferSize);
    return true;
}

bool BufferRing::Finalize(uint64_t descriptorsIOVABase, uint64_t buffersIOVABase) noexcept {
    if (descriptors_.empty() || buffers_.empty() || bufferCount_ == 0 || bufferSize_ == 0) {
        ASFW_LOG(Async, "BufferRing::Finalize: ring not initialized");
        return false;
    }
    if ((descriptorsIOVABase & 0xFULL) != 0 || (buffersIOVABase & 0xFULL) != 0) {
        ASFW_LOG(Async, "BufferRing::Finalize: device bases not 16-byte aligned (desc=0x%llx buf=0x%llx)", descriptorsIOVABase, buffersIOVABase);
        return false;
    }
    for (size_t i = 0; i < bufferCount_; ++i) {
        auto& desc = descriptors_[i];
        const uint64_t dataIOVA = buffersIOVABase + static_cast<uint64_t>(i) * bufferSize_;
        if (dataIOVA > 0xFFFFFFFFu) {
            ASFW_LOG(Async, "BufferRing::Finalize: buffer IOVA out of range (index=%zu iova=0x%llx)", i, dataIOVA);
            return false;
        }
        desc.dataAddress = static_cast<uint32_t>(dataIOVA);
        const size_t nextIndex = (i + 1) % bufferCount_;
        const uint64_t nextDescIOVA = descriptorsIOVABase + static_cast<uint64_t>(nextIndex) * sizeof(HW::OHCIDescriptor);
        const uint32_t branchWord = HW::MakeBranchWordAR(nextDescIOVA, /*continueFlag=*/true);
        if (branchWord == 0) {
            ASFW_LOG(Async, "BufferRing::Finalize: invalid branchWord for index %zu (nextIOVA=0x%llx)", i, nextDescIOVA);
            return false;
        }
        desc.branchWord = branchWord;
    }
    ASFW_LOG(Async, "BufferRing finalized: descIOVA=0x%llx bufIOVA=0x%llx buffers=%zu", descriptorsIOVABase, buffersIOVABase, bufferCount_);
    descIOVABase_ = static_cast<uint32_t>(descriptorsIOVABase & 0xFFFFFFFFu);
    bufIOVABase_ = static_cast<uint32_t>(buffersIOVABase & 0xFFFFFFFFu);
    return true;
}

std::optional<FilledBufferInfo> BufferRing::Dequeue() noexcept {
    if (descriptors_.empty()) {
        return std::nullopt;
    }

    size_t index = head_;

    // CRITICAL: Auto-recycling logic for AR DMA stream semantics
    // Per OHCI §3.3, §8.4.2 bufferFill mode: hardware fills current buffer until exhausted,
    // then advances to next. We detect this and recycle automatically.

    // Check if next descriptor has data (hardware advanced)
    const size_t next_index = (index + 1) % bufferCount_;
    auto& next_desc = descriptors_[next_index];

    if (dma_) {
        dma_->FetchFromDevice(&next_desc, sizeof(next_desc));
    }

    const uint16_t next_resCount = HW::AR_resCount(next_desc);
    const uint16_t next_reqCount = static_cast<uint16_t>(next_desc.control & 0xFFFF);

    // If next buffer has data, hardware has moved to it
    if (next_resCount != next_reqCount) {
        // Hardware advanced to next buffer! Recycle current buffer.
        ASFW_LOG_V4(Async,
                    "🔄 BufferRing::Dequeue: Hardware advanced to buffer[%zu] (resCount=%u/%u). "
                    "Auto-recycling buffer[%zu]...",
                    next_index, next_resCount, next_reqCount, index);

        // Recycle current buffer (resets resCount=reqCount)
        auto& desc_to_recycle = descriptors_[index];
        const uint16_t reqCount_recycle = static_cast<uint16_t>(desc_to_recycle.control & 0xFFFF);
        HW::AR_init_status(desc_to_recycle, reqCount_recycle);

        if (dma_) {
            dma_->PublishToDevice(&desc_to_recycle, sizeof(desc_to_recycle));
        }
        Driver::WriteBarrier();

        // Advance to next buffer
        head_ = next_index;
        last_dequeued_bytes_ = 0;  // Reset tracking for new buffer
        index = next_index;  // Process the new buffer now

        ASFW_LOG_V4(Async,
                    "✅ BufferRing: Auto-recycled buffer, advanced head_ →%zu",
                    index);
    }

    // Now process current buffer (either same as before, or newly advanced)
    auto& desc = descriptors_[index];

    // CRITICAL FIX: Invalidate CPU cache before reading descriptor status
    // Hardware wrote statusWord via DMA, must fetch fresh data to avoid reading stale cache
    if (dma_) {
        dma_->FetchFromDevice(&desc, sizeof(desc));
    }

    // CRITICAL: Do NOT add ReadBarrier() after FetchRange for uncached device memory!
    // For uncached memory, IoBarrier (DSB) is sufficient. Adding DMB may cause issues.

    #ifndef ASFW_HOST_TEST
    if (DMAMemoryManager::IsTracingEnabled()) {
        ASFW_LOG_V4(Async,
                    "  🔍 BufferRing::Dequeue: ReadBarrier NOT used (uncached device memory, DSB sufficient)");
    }
    #endif

    // Extract resCount and xferStatus using AR-specific accessors
    // CRITICAL: statusWord is in BIG-ENDIAN per OHCI §8.4.2, Table 8-1
    const uint16_t resCount = HW::AR_resCount(desc);
    const uint16_t reqCount = static_cast<uint16_t>(desc.control & 0xFFFF);

    // Calculate total bytes filled by hardware
    const size_t total_bytes_in_buffer = (resCount <= reqCount) ? (reqCount - resCount) : 0;

    // CRITICAL: AR DMA stream semantics (OHCI §3.3, §8.4.2 bufferFill mode)
    // Hardware ACCUMULATES multiple packets in the SAME buffer, raising an interrupt
    // after EACH packet. We must return only the NEW bytes since last Dequeue().
    
    // Check if there are NEW bytes beyond what we've already returned
    if (total_bytes_in_buffer <= last_dequeued_bytes_) {
        // No new data since last call
        return std::nullopt;
    }

    // Calculate NEW bytes that appeared since last Dequeue()
    const size_t start_offset = last_dequeued_bytes_;
    const size_t new_bytes = total_bytes_in_buffer - last_dequeued_bytes_;

    // Validate resCount sanity
    if (resCount > reqCount) {
        ASFW_LOG(Async, "BufferRing::Dequeue: invalid resCount %u > reqCount %u at index %zu",
                 resCount, reqCount, index);
        return std::nullopt;
    }

    #ifndef ASFW_HOST_TEST
    if (DMAMemoryManager::IsTracingEnabled()) {
        ASFW_LOG_V4(Async,
                    "🧭 BufferRing::Dequeue idx=%zu desc=%p reqCount=%u resCount=%u "
                    "total=%zu last_dequeued=%zu startOffset=%zu newBytes=%zu",
                    index, &desc, reqCount, resCount,
                    total_bytes_in_buffer, last_dequeued_bytes_, start_offset, new_bytes);
    }
    #endif

    // Get virtual address of buffer START (caller will add startOffset)
    void* bufferAddr = GetBufferAddress(index);
    if (!bufferAddr) {
        ASFW_LOG(Async, "BufferRing::Dequeue: invalid buffer address at index %zu", index);
        return std::nullopt;
    }

    // CRITICAL FIX: Invalidate buffer cache ONLY for the NEW bytes
    if (dma_) {
        auto* byte_ptr = static_cast<uint8_t*>(bufferAddr);
        dma_->FetchFromDevice(byte_ptr + start_offset, new_bytes);
    }

    // Update tracking: remember how many total bytes we've now returned
    last_dequeued_bytes_ = total_bytes_in_buffer;

    return FilledBufferInfo{
        .virtualAddress = bufferAddr,
        .startOffset = start_offset,
        .bytesFilled = total_bytes_in_buffer,  // Total bytes (caller parses [startOffset, bytesFilled))
        .descriptorIndex = index
    };
}

kern_return_t BufferRing::Recycle(size_t index) noexcept {
    // Validate index is current head
    if (index != head_) {
        ASFW_LOG(Async, "BufferRing::Recycle: index %zu != head %zu (out-of-order recycle)",
                 index, head_);
        return kIOReturnBadArgument;
    }

    if (index >= bufferCount_) {
        ASFW_LOG(Async, "BufferRing::Recycle: index %zu out of bounds", index);
        return kIOReturnBadArgument;
    }

    auto& desc = descriptors_[index];
    const uint16_t reqCount = static_cast<uint16_t>(desc.control & 0xFFFF);

    // DIAGNOSTIC: Read descriptor state BEFORE reset
    const uint16_t resCountBefore = HW::AR_resCount(desc);
    const uint16_t xferStatusBefore = HW::AR_xferStatus(desc);
    const uint32_t statusWordBefore = desc.statusWord;

    // Reset statusWord to indicate buffer is empty
    // CRITICAL: Use AR_init_status() to handle native byte order correctly
    HW::AR_init_status(desc, reqCount);

    // DIAGNOSTIC: Read descriptor state AFTER reset (but before cache flush)
    const uint16_t resCountAfter = HW::AR_resCount(desc);
    const uint16_t xferStatusAfter = HW::AR_xferStatus(desc);
    const uint32_t statusWordAfter = desc.statusWord;

    // Sync descriptor to device after AR_init_status (publish to HC)
    if (dma_) {
        dma_->PublishToDevice(&desc, sizeof(desc));
    }
    Driver::WriteBarrier();

    // CRITICAL DIAGNOSTIC: Always log recycle operation to trace buffer lifecycle
    ASFW_LOG_V4(Async,
                "♻️  BufferRing::Recycle[%zu]: BEFORE statusWord=0x%08X (resCount=%u xferStatus=0x%04X)",
                index, statusWordBefore, resCountBefore, xferStatusBefore);
    ASFW_LOG_V4(Async,
                "♻️  BufferRing::Recycle[%zu]: AFTER  statusWord=0x%08X (resCount=%u xferStatus=0x%04X) reqCount=%u",
                index, statusWordAfter, resCountAfter, xferStatusAfter, reqCount);
    ASFW_LOG_V4(Async,
                "♻️  BufferRing::Recycle[%zu]: head_ %zu → %zu (next buffer)",
                index, head_, (head_ + 1) % bufferCount_);

    if (resCountAfter != reqCount) {
        ASFW_LOG(Async,
                 "⚠️  BufferRing::Recycle[%zu]: UNEXPECTED! resCount=%u after reset, expected %u",
                 index, resCountAfter, reqCount);
    }

    #ifndef ASFW_HOST_TEST
    if (DMAMemoryManager::IsTracingEnabled()) {
        ASFW_LOG_V4(Async,
                    "🧭BufferRing::Recycle idx=%zu desc=%p reqCount=%u",
                    index,
                    &desc,
                    reqCount);
    }
    #endif

    // Advance head to next buffer (circular)
    head_ = (head_ + 1) % bufferCount_;

    // CRITICAL: Reset stream tracking for new buffer
    last_dequeued_bytes_ = 0;

    ASFW_LOG_V4(Async,
                "♻️  BufferRing::Recycle[%zu]: Advanced to next buffer, reset last_dequeued_bytes_=0",
                index);

    return kIOReturnSuccess;
}

void* BufferRing::GetBufferAddress(size_t index) const noexcept {
    if (index >= bufferCount_) return nullptr;
    const size_t offset = index * bufferSize_;
    if (offset + bufferSize_ > buffers_.size()) return nullptr;
    return buffers_.data() + offset;
}

uint32_t BufferRing::CommandPtrWord() const noexcept {
    if (descIOVABase_ == 0) return 0;
    return HW::MakeBranchWordAR(static_cast<uint64_t>(descIOVABase_), 1);
}

void BufferRing::BindDma(IDMAMemory* dma) noexcept { dma_ = dma; }

void BufferRing::PublishAllDescriptorsOnce() noexcept {
    if (!dma_ || descriptors_.empty()) return;
    dma_->PublishToDevice(descriptors_.data(), descriptors_.size_bytes());
    ::ASFW::Driver::IoBarrier();
}

} // namespace ASFW::Shared
