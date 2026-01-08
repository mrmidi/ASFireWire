#include "IsochDMAMemoryManager.hpp"
#include "../../Logging/Logging.hpp"
#include <new>

namespace ASFW::Isoch::Memory {

static constexpr size_t kMinDescriptorAlign = 16;
static constexpr size_t kDescriptorBudgetBytes = 64; // conservative: covers common OHCI descriptor variants
static constexpr size_t kMinSlabRounding = 4096;     // safe minimum; payload alignment handled via AlignCursorToIOVA

IsochDMAMemoryManager::IsochDMAMemoryManager(const IsochMemoryConfig& cfg) : cfg_(cfg) {
    if (cfg_.descriptorAlignment == 0) cfg_.descriptorAlignment = kMinDescriptorAlign;
    if (cfg_.payloadPageAlignment == 0) cfg_.payloadPageAlignment = 16384;
}

IsochDMAMemoryManager::~IsochDMAMemoryManager() {
    // DMAMemoryManager destructors call Reset() automatically
}

std::shared_ptr<IsochDMAMemoryManager> IsochDMAMemoryManager::Create(const IsochMemoryConfig& cfg) {
    auto ptr = std::shared_ptr<IsochDMAMemoryManager>(new (std::nothrow) IsochDMAMemoryManager(cfg));
    if (!ptr) return nullptr;

    if (!ptr->ValidateConfig()) {
        ASFW_LOG(Isoch, "IsochDMAMemoryManager: invalid config");
        return nullptr;
    }
    return ptr;
}

bool IsochDMAMemoryManager::IsPowerOf2(size_t v) noexcept {
    return v != 0 && ((v & (v - 1)) == 0);
}

size_t IsochDMAMemoryManager::RoundUp(size_t v, size_t align) noexcept {
    if (align == 0) return v;
    return (v + (align - 1)) & ~(align - 1);
}

bool IsochDMAMemoryManager::ValidateConfig() const noexcept {
    if (cfg_.numDescriptors == 0 || cfg_.packetSizeBytes == 0) {
        return false;
    }
    if (cfg_.descriptorAlignment < kMinDescriptorAlign) {
        return false;
    }
    if (!IsPowerOf2(cfg_.descriptorAlignment) || !IsPowerOf2(cfg_.payloadPageAlignment)) {
        return false;
    }
    return true;
}

bool IsochDMAMemoryManager::Initialize(ASFW::Driver::HardwareInterface& hw) {
    if (initialized_) {
        ASFW_LOG(Isoch, "IsochDMAMemoryManager: already initialized");
        return false;
    }
    if (!ValidateConfig()) {
        ASFW_LOG(Isoch, "IsochDMAMemoryManager: Initialize failed: bad config");
        return false;
    }

    // Descriptor slab: budget per descriptor, round to pages.
    // Alignment headroom logic: add cfg_.descriptorAlignment - 1 to be safe
    size_t descBytesRaw = 0;
    if (__builtin_mul_overflow(cfg_.numDescriptors, kDescriptorBudgetBytes, &descBytesRaw)) {
         ASFW_LOG(Isoch, "IsochDMAMemoryManager: desc size overflow");
         return false;
    }
    const size_t descHeaderRoom = (cfg_.descriptorAlignment > 0) ? (cfg_.descriptorAlignment - 1) : 0;
    const size_t descSlabBytes = RoundUp(descBytesRaw + descHeaderRoom, kMinSlabRounding);

    // Payload slab: exact ring length in bytes, plus headroom for IOVA alignment.
    size_t payloadBytesRaw = 0;
    if (__builtin_mul_overflow(cfg_.numDescriptors, cfg_.packetSizeBytes, &payloadBytesRaw)) {
         ASFW_LOG(Isoch, "IsochDMAMemoryManager: payload size overflow");
         return false;
    }
    const size_t payloadSlabBytes = RoundUp(payloadBytesRaw + (cfg_.payloadPageAlignment - 1), kMinSlabRounding);


    ASFW_LOG(Isoch,
             "IsochDMAMemoryManager: Initialize desc=%zu bytes payload=%zu bytes (payloadAlign=%zu)",
             descSlabBytes, payloadSlabBytes, cfg_.payloadPageAlignment);

    // Initialize descriptor slab
    if (!descMgr_.Initialize(hw, descSlabBytes)) {
        ASFW_LOG(Isoch, "IsochDMAMemoryManager: descMgr.Initialize failed");
        return false;
    }
    ASFW_LOG(Isoch,
             "IsochDMAMemoryManager: Descriptor slab - vaddr=%p iova=0x%llx size=%zu",
             descMgr_.BaseVirtual(), descMgr_.BaseIOVA(), descMgr_.TotalSize());

    // Initialize payload slab
    if (!payloadMgr_.Initialize(hw, payloadSlabBytes)) {
        ASFW_LOG(Isoch, "IsochDMAMemoryManager: payloadMgr.Initialize failed");
        descMgr_.Reset();
        return false;
    }
    ASFW_LOG(Isoch,
             "IsochDMAMemoryManager: Payload slab - vaddr=%p iova=0x%llx size=%zu",
             payloadMgr_.BaseVirtual(), payloadMgr_.BaseIOVA(), payloadMgr_.TotalSize());

    // Payload base alignment
    if (!payloadMgr_.AlignCursorToIOVA(cfg_.payloadPageAlignment)) {
        ASFW_LOG(Isoch, "IsochDMAMemoryManager: AlignCursorToIOVA(%zu) failed", cfg_.payloadPageAlignment);
        payloadMgr_.Reset();
        descMgr_.Reset();
        return false;
    }
    
    // Safety check: ensure we still have enough space after alignment
    if (payloadMgr_.AvailableSize() < payloadBytesRaw) {
        ASFW_LOG(Isoch, "IsochDMAMemoryManager: payload slab too small after alignment (need %zu, have %zu)", 
                 payloadBytesRaw, payloadMgr_.AvailableSize());
        payloadMgr_.Reset();
        descMgr_.Reset();
        return false;
    }

    // Descriptor base alignment (optional but good practice)
    // If descriptorAlignment > 16, we should align cursor.
    // Default slab alloc is 64-aligned, so usually fine for 16/32/64.
    if (cfg_.descriptorAlignment > 64) {
         if (!descMgr_.AlignCursorToIOVA(cfg_.descriptorAlignment)) {
            ASFW_LOG(Isoch, "IsochDMAMemoryManager: descMgr AlignCursorToIOVA(%zu) failed", cfg_.descriptorAlignment);
            payloadMgr_.Reset();
            descMgr_.Reset();
            return false;
         }
    }

    initialized_ = true;
    ASFW_LOG(Isoch, "IsochDMAMemoryManager: Initialization complete - ready for allocation");
    return true;
}

std::optional<ASFW::Shared::DMARegion> IsochDMAMemoryManager::AllocateDescriptor(size_t bytes) {
    if (!initialized_) return std::nullopt;
    auto r = descMgr_.AllocateRegion(bytes, cfg_.descriptorAlignment);
    if (!r) return std::nullopt;

    return ASFW::Shared::DMARegion{
        .virtualBase = r->virtualBase,
        .deviceBase  = r->deviceBase,
        .size        = r->size
    };
}

std::optional<ASFW::Shared::DMARegion> IsochDMAMemoryManager::AllocatePayloadBuffer(size_t bytes) {
    if (!initialized_) return std::nullopt;

    // Packet buffers themselves just need normal alignment; base alignment is already guaranteed by AlignCursorToIOVA.
    auto r = payloadMgr_.AllocateRegion(bytes, 16);
    if (!r) return std::nullopt;

    return ASFW::Shared::DMARegion{
        .virtualBase = r->virtualBase,
        .deviceBase  = r->deviceBase,
        .size        = r->size
    };
}

// IDMAMemory: trap generic allocation to force explicit APIs.
std::optional<ASFW::Shared::DMARegion> IsochDMAMemoryManager::AllocateRegion(size_t, size_t) {
    ASFW_LOG(Isoch, "IsochDMAMemoryManager: AllocateRegion() forbidden; use AllocateDescriptor/AllocatePayloadBuffer");
    return std::nullopt;
}

uint64_t IsochDMAMemoryManager::VirtToIOVA(const void* virt) const noexcept {
    const uint64_t d = descMgr_.VirtToIOVA(virt);
    if (d != 0) return d;
    return payloadMgr_.VirtToIOVA(virt);
}

void* IsochDMAMemoryManager::IOVAToVirt(uint64_t iova) const noexcept {
    void* d = descMgr_.IOVAToVirt(iova);
    if (d != nullptr) return d;
    return payloadMgr_.IOVAToVirt(iova);
}

void IsochDMAMemoryManager::PublishToDevice(const void* address, size_t length) const noexcept {
    if (address == nullptr || length == 0) {
        ::ASFW::Driver::IoBarrier();
        return;
    }
    // DMAMemoryManager provides cache/barrier logic
    if (descMgr_.VirtToIOVA(address) != 0) {
        descMgr_.PublishRange(address, length);
        return;
    }
    if (payloadMgr_.VirtToIOVA(address) != 0) {
        payloadMgr_.PublishRange(address, length);
        return;
    }
    ::ASFW::Driver::IoBarrier();
}

void IsochDMAMemoryManager::FetchFromDevice(const void* address, size_t length) const noexcept {
    if (address == nullptr || length == 0) {
        ::ASFW::Driver::IoBarrier();
        return;
    }
    if (descMgr_.VirtToIOVA(address) != 0) {
        descMgr_.FetchRange(address, length);
        return;
    }
    if (payloadMgr_.VirtToIOVA(address) != 0) {
        payloadMgr_.FetchRange(address, length);
        return;
    }
    ::ASFW::Driver::IoBarrier();
}

size_t IsochDMAMemoryManager::TotalSize() const noexcept {
    return descMgr_.TotalSize() + payloadMgr_.TotalSize();
}

size_t IsochDMAMemoryManager::AvailableSize() const noexcept {
    return descMgr_.AvailableSize() + payloadMgr_.AvailableSize();
}

} // namespace ASFW::Isoch::Memory
