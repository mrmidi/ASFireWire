#include "PayloadHandle.hpp"
#include "../Core/DMAMemoryManager.hpp"

namespace ASFW::Async {

PayloadHandle::~PayloadHandle() noexcept {
    Release();
}

void PayloadHandle::Release() noexcept {
    // Phase 2.0: DMAMemoryManager is a slab allocator with no individual Free()
    // Memory is reclaimed when the entire slab is destroyed during AsyncSubsystem shutdown
    // Just clear the handle state
    if (address_ != 0) {
        dmaMgr_ = nullptr;
        address_ = 0;
        size_ = 0;
        physAddr_ = 0;
    }
}

} // namespace ASFW::Async
