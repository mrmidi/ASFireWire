#include "DMAMemoryManager.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <DriverKit/IOLib.h>

#include "../../Hardware/HardwareInterface.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Common/BarrierUtils.hpp"

namespace ASFW::Shared {

namespace {
constexpr size_t kTracePreviewBytes = 64;
std::atomic<bool> gDMACoherencyTraceEnabled{false};
} // namespace

void DMAMemoryManager::SetTracingEnabled(bool enabled) noexcept {
    const bool previous = gDMACoherencyTraceEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) {
        return;
    }
    ASFW_LOG(Async, "DMAMemoryManager: coherency tracing %{public}s",
             enabled ? "ENABLED" : "disabled");
}

bool DMAMemoryManager::IsTracingEnabled() noexcept {
    return gDMACoherencyTraceEnabled.load(std::memory_order_acquire);
}

DMAMemoryManager::~DMAMemoryManager() { Reset(); }

void DMAMemoryManager::Reset() noexcept {
    // Release CPU mapping first
    if (dmaMemoryMap_) {
        dmaMemoryMap_->release();
        dmaMemoryMap_ = nullptr;
    }

    // Tear down IOMMU mapping next
    if (dmaCommand_) {
        dmaCommand_->CompleteDMA(kIODMACommandCompleteDMANoOptions);
        dmaCommand_.reset();
    }

    // Release backing buffer last
    dmaBuffer_.reset();

    slabVirt_ = nullptr;
    slabIOVA_ = 0;
    slabSize_ = 0;
    mappingLength_ = 0;
    cursor_ = 0;
}

bool DMAMemoryManager::Initialize(Driver::HardwareInterface& hw, size_t totalSize) {
    ASFW_LOG(Async, "DMAMemoryManager: Initializing with totalSize=%zu", totalSize);
    
    if (slabVirt_ != nullptr) {
        ASFW_LOG(Async, "DMAMemoryManager: Already initialized");
        return false;
    }

    if (totalSize == 0) {
        ASFW_LOG_ERROR(Async, "DMAMemoryManager::Initialize: totalSize=0");
        return false;
    }

    // Enforce 16-byte alignment per OHCI §1.7
    const size_t alignedSize = AlignSize(totalSize);

    ASFW_LOG(Async, "DMAMemoryManager: Allocating %zu bytes (requested %zu)",
             alignedSize, totalSize);

    // Allocate DMA buffer via HardwareInterface
    auto dmaBufferOpt = hw.AllocateDMA(alignedSize, kIOMemoryDirectionInOut);
    if (!dmaBufferOpt.has_value()) {
        ASFW_LOG(Async, "DMAMemoryManager: AllocateDMA failed for %zu bytes", alignedSize);
        return false;
    }

    auto& dmaBufferInfo = dmaBufferOpt.value();
    dmaBuffer_ = dmaBufferInfo.descriptor;
    dmaCommand_ = dmaBufferInfo.dmaCommand;  // CRITICAL: Keep alive for IOMMU mapping
    slabIOVA_ = dmaBufferInfo.deviceAddress;
    mappingLength_ = dmaBufferInfo.length;

    // Validate physical address fits in 32-bit space (OHCI requirement)
    if (slabIOVA_ > 0xFFFFFFFFULL) {
        ASFW_LOG(Async, "DMAMemoryManager: IOVA 0x%llx exceeds 32-bit range", slabIOVA_);
        return false;
    }

    // Validate 16-byte alignment
    if ((slabIOVA_ & 0xF) != 0) {
        ASFW_LOG(Async, "DMAMemoryManager: IOVA 0x%llx not 16-byte aligned", slabIOVA_);
        return false;
    }

    // Create uncached mapping (cache-inhibit mode)
    // macOS DriverKit reliably supports this mode - writes bypass CPU cache
    kern_return_t kr = dmaBuffer_->CreateMapping(
        /*options*/  kIOMemoryMapCacheModeInhibit,  // Uncached mapping
        /*address*/  0,
        /*offset*/   0,
        /*length*/   alignedSize,
        /*alignment*/0,
        &dmaMemoryMap_);
    
    if (kr != kIOReturnSuccess || dmaMemoryMap_ == nullptr) {
        ASFW_LOG_ERROR(Async, "DMAMemoryManager: CreateMapping failed, kr=0x%08x", kr);
        return false;
    }

    // CRITICAL: Use CPU mapping's actual length, not DMA/IOMMU segment length
    mappingLength_ = static_cast<size_t>(dmaMemoryMap_->GetLength());
    if (mappingLength_ < alignedSize) {
        ASFW_LOG_ERROR(Async,
            "DMAMemoryManager::Initialize: CPU map shorter than requested: mapLen=%zu < need=%zu",
            mappingLength_, alignedSize);
        return false;
    }

    slabVirt_ = reinterpret_cast<uint8_t*>(dmaMemoryMap_->GetAddress());
    if (slabVirt_ == nullptr) {
        ASFW_LOG(Async, "DMAMemoryManager: Mapping returned null virtual address");
        return false;
    }

    // Verify mapping is writable (sanity probe)
    volatile uint8_t* probe = slabVirt_;
    uint8_t tmp = *probe;  // Read must work
    *(const_cast<uint8_t*>(probe)) = tmp;  // Tiny write; if this faults, mapping is RO

    // Get DMA/IOMMU address from segments (for device-visible address)
    if (dmaCommand_) {
#if defined(IODMACommand_GetSegments_ID)
        IOAddressSegment segment{};
        uint32_t count = 1;
        kern_return_t segKr = dmaCommand_->GetSegments(&segment, &count);
        if (segKr == kIOReturnSuccess && count >= 1) {
            slabIOVA_ = segment.address;
        } else {
            ASFW_LOG(Async,
                     "DMAMemoryManager: GetSegments failed (kr=0x%08x count=%u) — using allocation metadata",
                     segKr,
                     count);
        }
#endif
    }

    slabSize_ = alignedSize;
    cursor_ = 0;

    // Zero entire slab for deterministic descriptor state
    ZeroSlab(slabSize_);

    ASFW_LOG(Async,
             "DMAMemoryManager: Initialized - vaddr=%p iova=0x%llx size=%zu mapped=%zu",
             slabVirt_, slabIOVA_, slabSize_, mappingLength_);
    ASFW_LOG(Async,
             "  Cache mode: UNCACHED (kIOMemoryMapCacheModeInhibit)");
    ASFW_LOG(Async,
             "  Alignment: 16B (OHCI §1.7), CPU writes bypass cache → RAM directly");

    return true;
}

std::optional<DMAMemoryManager::Region> DMAMemoryManager::AllocateRegion(size_t size) {
    if (slabVirt_ == nullptr) {
        ASFW_LOG(Async, "DMAMemoryManager: AllocateRegion called before Initialize");
        return std::nullopt;
    }

    if (size == 0) {
        ASFW_LOG(Async, "DMAMemoryManager: AllocateRegion with size=0");
        return std::nullopt;
    }

    // Enforce 16-byte alignment
    const size_t alignedSize = AlignSize(size);

    if (cursor_ + alignedSize > slabSize_) {
        ASFW_LOG_ERROR(Async,
            "DMAMemoryManager: AllocateRegion would overflow - need %zu, have %zu (slab=%zu cursor=%zu)",
            alignedSize, slabSize_ - cursor_, slabSize_, cursor_);
        return std::nullopt;
    }

    Region region{};
    region.virtualBase = slabVirt_ + cursor_;
    region.deviceBase = slabIOVA_ + cursor_;
    region.size = alignedSize;

    cursor_ += alignedSize;

    ASFW_LOG(Async, "DMAMemoryManager: Allocated region - vaddr=%p iova=0x%llx size=%zu (requested %zu)",
             region.virtualBase, region.deviceBase, region.size, size);

    return region;
}

uint64_t DMAMemoryManager::VirtToIOVA(const void* virt) const noexcept {
    if (!IsInSlabRange(virt)) {
        return 0;
    }

    const auto* bytePtr = static_cast<const uint8_t*>(virt);
    const ptrdiff_t offset = bytePtr - slabVirt_;

    return slabIOVA_ + static_cast<uint64_t>(offset);
}

void* DMAMemoryManager::IOVAToVirt(uint64_t iova) const noexcept {
    if (!IsInSlabRange(iova)) {
        return nullptr;
    }

    const uint64_t offset = iova - slabIOVA_;

    // Additional bounds check after offset calculation
    if (offset >= slabSize_) {
        return nullptr;
    }

    return slabVirt_ + offset;
}

bool DMAMemoryManager::IsInSlabRange(const void* ptr) const noexcept {
    if (slabVirt_ == nullptr || ptr == nullptr) {
        return false;
    }

    const auto* bytePtr = static_cast<const uint8_t*>(ptr);
    return (bytePtr >= slabVirt_) && (bytePtr < (slabVirt_ + slabSize_));
}

bool DMAMemoryManager::IsInSlabRange(uint64_t iova) const noexcept {
    if (slabIOVA_ == 0 || iova == 0) {
        return false;
    }

    return (iova >= slabIOVA_) && (iova < (slabIOVA_ + slabSize_));
}

void DMAMemoryManager::ZeroSlab(size_t length) noexcept {
    if (slabVirt_ == nullptr || length == 0) {
        return;
    }

    const size_t cappedLength = std::min(length, slabSize_);

    // Cache-inhibited mappings reject dc zva; use plain stores via volatile pointer
    auto* volatilePtr = reinterpret_cast<volatile uint8_t*>(slabVirt_);
    for (size_t i = 0; i < cappedLength; ++i) {
        volatilePtr[i] = 0;
    }
}

void DMAMemoryManager::PublishRange(const void* address, size_t length) const noexcept {
    if (address == nullptr || length == 0) {
        ::ASFW::Driver::IoBarrier();
        return;
    }

    if (!IsInSlabRange(address)) {
        if (IsTracingEnabled()) {
            ASFW_LOG(Async,
                     "⚠️  PublishRange ignored: address %p (len=%zu) outside DMA slab",
                     address, length);
        }
        ::ASFW::Driver::IoBarrier();
        return;
    }

    if (IsTracingEnabled()) {
        TraceHexPreview("PublishRange", address, length);
        ASFW_LOG(Async,
                 "🧭 PublishRange: virt=%p len=%zu (uncached - barrier only)",
                 address, length);
    }

    // Uncached mode: CPU writes bypass cache, just need ordering barrier
    ::ASFW::Driver::IoBarrier();
}

void DMAMemoryManager::FetchRange(const void* address, size_t length) const noexcept {
    if (address == nullptr || length == 0) {
        ::ASFW::Driver::IoBarrier();
        return;
    }

    if (!IsInSlabRange(address)) {
        if (IsTracingEnabled()) {
            ASFW_LOG(Async,
                     "⚠️  FetchRange ignored: address %p (len=%zu) outside DMA slab",
                     address, length);
        }
        ::ASFW::Driver::IoBarrier();
        return;
    }

    // Uncached mode: CPU reads see latest writes, just need ordering barrier
    ::ASFW::Driver::IoBarrier();

    if (IsTracingEnabled()) {
        TraceHexPreview("FetchRange", address, length);
        ASFW_LOG(Async,
                 "🧭 FetchRange: virt=%p len=%zu (uncached - barrier only)",
                 address, length);
    }
}

void DMAMemoryManager::TraceHexPreview(const char* tag,
                                       const void* address,
                                       size_t length) const noexcept {
    if (!IsTracingEnabled() || address == nullptr || length == 0) {
        return;
    }

    const auto* bytes = static_cast<const uint8_t*>(address);
    const size_t preview = std::min(length, kTracePreviewBytes);
    char line[3 * 16 + 1];

    for (size_t offset = 0; offset < preview; offset += 16) {
        const size_t chunk = std::min(static_cast<size_t>(16), preview - offset);
        char* cursor = line;
        size_t remaining = sizeof(line);
        for (size_t i = 0; i < chunk && remaining > 3; ++i) {
            const int written = std::snprintf(cursor, remaining, "%02X ", bytes[offset + i]);
            if (written <= 0) {
                break;
            }
            cursor += written;
            remaining -= static_cast<size_t>(written);
        }
        *cursor = '\0';
        ASFW_LOG(Async,
                 "    %{public}s +0x%02zx: %{public}s",
                 tag,
                 offset,
                 line);
    }
}

void DMAMemoryManager::HexDump64(const void* address, const char* tag) const noexcept {
    // Align to 64-byte cache line boundary
    const auto* d = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(address) & ~uintptr_t(63));
    
    ASFW_LOG(Async, "[%{public}s] 64B@%p:", tag, d);
    ASFW_LOG(Async, "  [00-1F] %08x %08x %08x %08x  %08x %08x %08x %08x",
             d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    ASFW_LOG(Async, "  [20-3F] %08x %08x %08x %08x  %08x %08x %08x %08x",
             d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
}

} // namespace ASFW::Shared
