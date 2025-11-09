#include "BufferRing.hpp"

#include <atomic>
#include <cstring>

#include "../../Core/BarrierUtils.hpp"
#include "../Core/DMAMemoryManager.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Async {

bool BufferRing::Initialize(
    std::span<HW::OHCIDescriptor> descriptors,
    std::span<uint8_t> buffers,
    size_t bufferCount,
    size_t bufferSize) noexcept {

    // Validate parameters
    if (descriptors.empty() || buffers.empty()) {
        ASFW_LOG(Async, "BufferRing::Initialize: empty storage");
        return false;
    }

    if (descriptors.size() != bufferCount) {
        ASFW_LOG(Async, "BufferRing::Initialize: descriptor count %zu != buffer count %zu",
                 descriptors.size(), bufferCount);
        return false;
    }

    if (buffers.size() < bufferCount * bufferSize) {
        ASFW_LOG(Async, "BufferRing::Initialize: buffer storage too small (%zu < %zu)",
                 buffers.size(), bufferCount * bufferSize);
        return false;
    }

    // Check descriptor alignment (OHCI §7.1: must be 16-byte aligned)
    if (reinterpret_cast<uintptr_t>(descriptors.data()) % 16 != 0) {
        ASFW_LOG(Async, "BufferRing::Initialize: descriptors not 16-byte aligned");
        return false;
    }

    descriptors_ = descriptors;
    buffers_ = buffers;
    bufferCount_ = bufferCount;
    bufferSize_ = bufferSize;
    head_ = 0;

    // Initialize INPUT_MORE descriptors in buffer-fill mode
    // Per OHCI §8.4.2, Table 8-1
    for (size_t i = 0; i < bufferCount; ++i) {
        auto& desc = descriptors_[i];

        // Zero descriptor first
        std::memset(&desc, 0, sizeof(desc));

        // Build control word per OHCI Table 8-1:
        // - cmd[31:28] = 0x2 (INPUT_MORE)
        // - key[27:25] = 0x0 (standard)
        // - s[24] = 1 (store xferStatus in statusWord)
        // - i[23:22] = 0b11 (always interrupt)
        // - b[21:20] = 0b11 (always branch)
        // - reserved[19:16] = 0
        // - reqCount[15:0] = bufferSize (HOST byte order)
        constexpr uint32_t kCmdInputMore = HW::OHCIDescriptor::kCmdInputMore;
        constexpr uint32_t kKeyStandard = HW::OHCIDescriptor::kKeyStandard;
        constexpr uint32_t kS = 1u;  // Store status
        constexpr uint32_t kIntAlways = HW::OHCIDescriptor::kIntAlways;
        constexpr uint32_t kBranchAlways = HW::OHCIDescriptor::kBranchAlways;

        desc.control = (kCmdInputMore << 28) |
                       (kKeyStandard << 25) |
                       (kS << 24) |
                       (kIntAlways << 22) |
                       (kBranchAlways << 20) |
                       static_cast<uint32_t>(bufferSize);

        // dataAddress points to this buffer's data
        // NOTE: This is a temporary placeholder - caller must update with physical addresses
        // For now, store offset which can be used to calculate virtual address
        desc.dataAddress = static_cast<uint32_t>(i * bufferSize);

        // branchWord links to next descriptor (or wraps to start)
        size_t nextIndex = (i + 1) % bufferCount;
        // NOTE: Physical address must be filled in by caller after DMA mapping
        // For now, store next index as placeholder (bits [31:4] will be updated later)
        // Z=1 indicates continue to next descriptor
        desc.branchWord = (1u << 28) | (static_cast<uint32_t>(nextIndex) << 4);

        // Initialize statusWord with AR_init_status()
        // CRITICAL: statusWord is in HOST byte order (native/little-endian on x86/ARM)
        // Format: [xferStatus:16][resCount:16] in native byte order
        // Initial state: xferStatus=0, resCount=reqCount (buffer empty)
        HW::AR_init_status(desc, static_cast<uint16_t>(bufferSize));
    }

    ASFW_LOG(Async, "BufferRing initialized: %zu buffers x %zu bytes", bufferCount, bufferSize);
    return true;
}

bool BufferRing::Finalize(uint64_t descriptorsIOVABase,
                          uint64_t buffersIOVABase) noexcept {
    if (descriptors_.empty() || buffers_.empty() || bufferCount_ == 0 || bufferSize_ == 0) {
        ASFW_LOG(Async, "BufferRing::Finalize: ring not initialized");
        return false;
    }

    if ((descriptorsIOVABase & 0xFULL) != 0 || (buffersIOVABase & 0xFULL) != 0) {
        ASFW_LOG(Async,
                 "BufferRing::Finalize: device bases not 16-byte aligned (desc=0x%llx buf=0x%llx)",
                 descriptorsIOVABase,
                 buffersIOVABase);
        return false;
    }

    for (size_t i = 0; i < bufferCount_; ++i) {
        auto& desc = descriptors_[i];

        const uint64_t dataIOVA = buffersIOVABase + static_cast<uint64_t>(i) * bufferSize_;
        if (dataIOVA > 0xFFFFFFFFu) {
            ASFW_LOG(Async,
                     "BufferRing::Finalize: buffer IOVA out of range (index=%zu iova=0x%llx)",
                     i,
                     dataIOVA);
            return false;
        }
        desc.dataAddress = static_cast<uint32_t>(dataIOVA);

        const size_t nextIndex = (i + 1) % bufferCount_;
        const uint64_t nextDescIOVA = descriptorsIOVABase +
                                      static_cast<uint64_t>(nextIndex) * sizeof(HW::OHCIDescriptor);
        const uint32_t branchWord = HW::MakeBranchWordAR(nextDescIOVA, /*continueFlag=*/true);
        if (branchWord == 0) {
            ASFW_LOG(Async,
                     "BufferRing::Finalize: invalid branchWord for index %zu (nextIOVA=0x%llx)",
                     i,
                     nextDescIOVA);
            return false;
        }
        desc.branchWord = branchWord;
    }

    ASFW_LOG(Async,
             "BufferRing finalized: descIOVA=0x%llx bufIOVA=0x%llx buffers=%zu",
             descriptorsIOVABase,
             buffersIOVABase,
             bufferCount_);
    // Record 32-bit device bases for later use when programming controller.
    // Caller should ensure addresses fit in 32-bit as required by OHCI.
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
    //
    // Per OHCI §3.3, §8.4.2 bufferFill mode:
    // - Hardware fills current buffer (head_) until resCount ≈ 0
    // - When buffer exhausted, hardware advances to next descriptor
    // - Software should detect this and recycle the old buffer
    //
    // Detection strategy:
    // - Check if NEXT descriptor (head_ + 1) has data (resCount != reqCount)
    // - If yes: Hardware has moved! Recycle current buffer and advance head_

    // Check if next descriptor has data (hardware advanced)
    const size_t next_index = (index + 1) % bufferCount_;
    auto& next_desc = descriptors_[next_index];

    if (dma_) {
        dma_->FetchRange(&next_desc, sizeof(next_desc));
    }

    const uint16_t next_resCount = HW::AR_resCount(next_desc);
    const uint16_t next_reqCount = static_cast<uint16_t>(next_desc.control & 0xFFFF);

    // If next buffer has data, hardware has moved to it
    if (next_resCount != next_reqCount) {
        // Hardware advanced to next buffer! Recycle current buffer.
        ASFW_LOG(Async,
                 "🔄 BufferRing::Dequeue: Hardware advanced to buffer[%zu] (resCount=%u/%u). "
                 "Auto-recycling buffer[%zu]...",
                 next_index, next_resCount, next_reqCount, index);

        // Recycle current buffer (resets resCount=reqCount)
        auto& desc_to_recycle = descriptors_[index];
        const uint16_t reqCount_recycle = static_cast<uint16_t>(desc_to_recycle.control & 0xFFFF);
        HW::AR_init_status(desc_to_recycle, reqCount_recycle);

        if (dma_) {
            dma_->PublishRange(&desc_to_recycle, sizeof(desc_to_recycle));
        }
        Driver::WriteBarrier();

        // Advance to next buffer
        head_ = next_index;
        last_dequeued_bytes_ = 0;  // Reset tracking for new buffer
        index = next_index;  // Process the new buffer now

        ASFW_LOG(Async,
                 "✅ BufferRing: Auto-recycled buffer, advanced head_ →%zu",
                 index);
    }

    // Now process current buffer (either same as before, or newly advanced)
    auto& desc = descriptors_[index];

    // CRITICAL FIX: Invalidate CPU cache before reading descriptor status
    // Hardware wrote statusWord via DMA, must fetch fresh data to avoid reading stale cache
    // Without this, we may read old resCount==reqCount (empty buffer) even after hardware filled it
    if (dma_) {
        dma_->FetchRange(&desc, sizeof(desc));
    }

    // CRITICAL: Do NOT add ReadBarrier() after FetchRange for uncached device memory!
    //
    // WHY: DMA descriptors are mapped as DEVICE MEMORY (kIOMemoryMapCacheModeInhibit)
    // - FetchRange() → IoBarrier() → DSB (Device Synchronization Barrier) for device memory
    // - ReadBarrier() → DMB (Data Memory Barrier) is for NORMAL MEMORY (cached)
    // - On ARM64, DMB does NOT synchronize with device memory accesses
    // - Adding DMB after DSB may allow CPU to reorder descriptor load before DSB completes
    // - Result: Read STALE speculative data instead of fresh hardware DMA writes
    //
    // For uncached device memory, IoBarrier (DSB) is sufficient. Adding ReadBarrier may
    // actually CAUSE cache coherency issues, not fix them!
    //
    // See: ANALYSIS_DMA_BARRIERS_AND_CACHE_COHERENCY.md for full technical explanation
    //
    // DO NOT UNCOMMENT: Driver::ReadBarrier();  // ⚠️  Wrong barrier type for device memory!

    if (DMAMemoryManager::IsTracingEnabled()) {
        ASFW_LOG(Async,
                 "  🔍 BufferRing::Dequeue: ReadBarrier NOT used (uncached device memory, DSB sufficient)");
    }

    // Extract resCount and xferStatus using AR-specific accessors
    // CRITICAL: statusWord is in BIG-ENDIAN per OHCI §8.4.2, Table 8-1
    const uint16_t resCount = HW::AR_resCount(desc);
    const uint16_t reqCount = static_cast<uint16_t>(desc.control & 0xFFFF);

    // Calculate total bytes filled by hardware
    // Per OHCI §8.4.2: resCount is decremented as bytes are written
    const size_t total_bytes_in_buffer = (resCount <= reqCount) ? (reqCount - resCount) : 0;

    // CRITICAL: AR DMA stream semantics (OHCI §3.3, §8.4.2 bufferFill mode)
    // Hardware ACCUMULATES multiple packets in the SAME buffer, raising an interrupt
    // after EACH packet. We must return only the NEW bytes that appeared since the
    // last Dequeue() call, not re-process old packets.
    //
    // Example: Buffer[0] accumulates 4 packets over 4 interrupts:
    //   Int#1: resCount=4140 → total=20 bytes, last_dequeued=0 → NEW=20 bytes (packet 1)
    //   Int#2: resCount=4120 → total=40 bytes, last_dequeued=20 → NEW=20 bytes (packet 2 APPENDED)
    //   Int#3: resCount=4100 → total=60 bytes, last_dequeued=40 → NEW=20 bytes (packet 3 APPENDED)
    //   Int#4: resCount=4080 → total=80 bytes, last_dequeued=60 → NEW=20 bytes (packet 4 APPENDED)

    // Check if there are NEW bytes beyond what we've already returned
    if (total_bytes_in_buffer <= last_dequeued_bytes_) {
        // No new data since last call - either:
        // 1. Buffer still empty (total_bytes_in_buffer == 0 == last_dequeued_bytes_)
        // 2. We already returned all available data in a previous Dequeue() call
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

    if (DMAMemoryManager::IsTracingEnabled()) {
        ASFW_LOG(Async,
                 "🧭 BufferRing::Dequeue idx=%zu desc=%p reqCount=%u resCount=%u "
                 "total=%zu last_dequeued=%zu startOffset=%zu newBytes=%zu",
                 index, &desc, reqCount, resCount,
                 total_bytes_in_buffer, last_dequeued_bytes_, start_offset, new_bytes);
    }

    // Get virtual address of buffer START (caller will add startOffset)
    void* bufferAddr = GetBufferAddress(index);
    if (!bufferAddr) {
        ASFW_LOG(Async, "BufferRing::Dequeue: invalid buffer address at index %zu", index);
        return std::nullopt;
    }

    // CRITICAL FIX: Invalidate buffer cache ONLY for the NEW bytes
    // Hardware wrote new packet data via DMA, must fetch fresh data
    if (dma_) {
        // Invalidate from start_offset to end of new data
        auto* byte_ptr = static_cast<uint8_t*>(bufferAddr);
        dma_->FetchRange(byte_ptr + start_offset, new_bytes);
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
    // Sets: xferStatus=0, resCount=reqCount (buffer ready for hardware)
    HW::AR_init_status(desc, reqCount);

    // DIAGNOSTIC: Read descriptor state AFTER reset (but before cache flush)
    const uint16_t resCountAfter = HW::AR_resCount(desc);
    const uint16_t xferStatusAfter = HW::AR_xferStatus(desc);
    const uint32_t statusWordAfter = desc.statusWord;

    // Sync descriptor to device after AR_init_status (publish to HC)
    if (dma_) {
        dma_->PublishRange(&desc, sizeof(desc));  // publish to HC
    }
    Driver::WriteBarrier();

    // CRITICAL DIAGNOSTIC: Always log recycle operation to trace buffer lifecycle
    ASFW_LOG(Async,
             "♻️  BufferRing::Recycle[%zu]: BEFORE statusWord=0x%08X (resCount=%u xferStatus=0x%04X)",
             index, statusWordBefore, resCountBefore, xferStatusBefore);
    ASFW_LOG(Async,
             "♻️  BufferRing::Recycle[%zu]: AFTER  statusWord=0x%08X (resCount=%u xferStatus=0x%04X) reqCount=%u",
             index, statusWordAfter, resCountAfter, xferStatusAfter, reqCount);
    ASFW_LOG(Async,
             "♻️  BufferRing::Recycle[%zu]: head_ %zu → %zu (next buffer)",
             index, head_, (head_ + 1) % bufferCount_);

    if (resCountAfter != reqCount) {
        ASFW_LOG(Async,
                 "⚠️  BufferRing::Recycle[%zu]: UNEXPECTED! resCount=%u after reset, expected %u",
                 index, resCountAfter, reqCount);
    }

    if (DMAMemoryManager::IsTracingEnabled()) {
        ASFW_LOG(Async,
                 "🧭 BufferRing::Recycle idx=%zu desc=%p reqCount=%u",
                 index,
                 &desc,
                 reqCount);
    }

    // Advance head to next buffer (circular)
    head_ = (head_ + 1) % bufferCount_;

    // CRITICAL: Reset stream tracking for new buffer
    // The new head_ buffer starts with zero bytes processed
    last_dequeued_bytes_ = 0;

    ASFW_LOG(Async,
             "♻️  BufferRing::Recycle[%zu]: Advanced to next buffer, reset last_dequeued_bytes_=0",
             index);

    return kIOReturnSuccess;
}

void* BufferRing::GetBufferAddress(size_t index) const noexcept {
    if (index >= bufferCount_) {
        return nullptr;
    }

    // Calculate offset into buffer storage
    const size_t offset = index * bufferSize_;

    // Validate offset is within buffer storage
    if (offset + bufferSize_ > buffers_.size()) {
        return nullptr;
    }

    return buffers_.data() + offset;
}

uint32_t BufferRing::CommandPtrWord() const noexcept {
    if (descIOVABase_ == 0) return 0;
    // Z=1 (continue) for AR continuous-run
    return HW::MakeBranchWordAR(static_cast<uint64_t>(descIOVABase_), /*zContinue=*/1);
}

void BufferRing::BindDma(class DMAMemoryManager* dma) noexcept {
    dma_ = dma;
}

void BufferRing::PublishAllDescriptorsOnce() noexcept {
    if (!dma_ || descriptors_.empty()) return;
    dma_->PublishRange(descriptors_.data(),
                       descriptors_.size_bytes());
    ::ASFW::Driver::IoBarrier();
}

} // namespace ASFW::Async
