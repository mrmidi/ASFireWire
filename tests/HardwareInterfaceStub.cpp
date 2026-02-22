#include "HardwareInterface.hpp"
#include "RegisterMap.hpp"

// Minimal stub implementation for unit tests
namespace ASFW::Driver {


HardwareInterface::HardwareInterface() = default;
HardwareInterface::~HardwareInterface() = default;

kern_return_t HardwareInterface::Attach(IOService*, IOService*) {
    return kIOReturnSuccess;
}

void HardwareInterface::Detach() {}

void HardwareInterface::SetAsyncSubsystem(ASFW::Async::AsyncSubsystem*) noexcept {}

uint32_t HardwareInterface::Read(Register32) const noexcept {
    return 0;
}

void HardwareInterface::Write(Register32, uint32_t) noexcept {
    // Stub - no-op for tests
}

std::pair<uint32_t, uint64_t> HardwareInterface::ReadCycleTimeAndUpTime() const noexcept {
    return {0, mach_absolute_time()};
}

std::optional<HardwareInterface::DMABuffer> HardwareInterface::AllocateDMA(size_t length, uint64_t options, size_t alignment) {
    IOBufferMemoryDescriptor* buffer = nullptr;
    if (IOBufferMemoryDescriptor::Create(options, length, alignment, &buffer) != kIOReturnSuccess) {
        return std::nullopt;
    }
    
    IODMACommand* cmd = new IODMACommand();
    
    IOAddressSegment seg;
    buffer->GetAddressRange(&seg);

    DMABuffer ret;
    ret.descriptor = OSSharedPtr<IOBufferMemoryDescriptor>(buffer, OSNoRetain);
    ret.dmaCommand = OSSharedPtr<IODMACommand>(cmd, OSNoRetain);
    ret.length = length;
    
    // CAS loop to allocate non-colliding aligned addresses
    static std::atomic<uint32_t> sMockIOVA{0x20000000};
    
    // Helper lambda for alignment
    auto alignUp32 = [](uint32_t v, uint32_t a) -> uint32_t {
        return (a == 0) ? v : ((v + (a - 1)) & ~(a - 1));
    };

    uint32_t a = (alignment == 0) ? 16u : static_cast<uint32_t>(alignment);
    // If not power of two (unlikely given HardwareInterface checks), fallback to 16
    if ((a & (a - 1)) != 0) a = 16u;

    // Advance by ALIGNED size. 
    // We want the allocation to be Size aligned, plus maybe some padding.
    // Actually we care that the BASE is aligned. 
    // sMockIOVA represents the next free byte.
    
    // We add 4096 padding between allocations to verify robust "hole" skipping logic if needed,
    // and to simulate typical page-aligned IOMMU behavior.
    
    uint32_t cur = sMockIOVA.load(std::memory_order_relaxed);
    uint32_t base = 0;
    
    for (;;) {
        base = alignUp32(cur, a); // Align the current cursor up to requested alignment
        uint32_t nextCandidate = base + static_cast<uint32_t>(length) + 4096u; // Advance + padding
        
        // Ensure no wrap around 32-bit (test safety limit)
        if (nextCandidate < base) {
             return std::nullopt; // simulate OOM in 32-bit IOVA space
        }

        if (sMockIOVA.compare_exchange_weak(cur, nextCandidate, std::memory_order_acq_rel)) {
            break;
        }
        // CAS failed, 'cur' is updated to new value, retry loop
    }
    
    ret.deviceAddress = base;
    return ret;
}

}
