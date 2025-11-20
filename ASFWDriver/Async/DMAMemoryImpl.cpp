#include "DMAMemoryImpl.hpp"

namespace ASFW::Async {

DMAMemoryImpl::DMAMemoryImpl(DMAMemoryManager& mgr) : mgr_(mgr) {}

std::optional<DMARegion> DMAMemoryImpl::AllocateRegion(size_t size, size_t alignment) {
    // Note: alignment parameter is currently ignored as DMAMemoryManager
    // enforces 16-byte alignment for all allocations (OHCI requirement).
    // Future enhancement: support custom alignment if needed.
    auto region = mgr_.AllocateRegion(size);
    if (!region) {
        return std::nullopt;
    }

    return DMARegion{
        .virtualBase = region->virtualBase,
        .deviceBase = region->deviceBase,
        .size = region->size
    };
}

uint64_t DMAMemoryImpl::VirtToIOVA(const void* virt) const noexcept {
    return mgr_.VirtToIOVA(virt);
}

void* DMAMemoryImpl::IOVAToVirt(uint64_t iova) const noexcept {
    return mgr_.IOVAToVirt(iova);
}

void DMAMemoryImpl::PublishToDevice(const void* address, size_t length) const noexcept {
    mgr_.PublishRange(address, length);
}

void DMAMemoryImpl::FetchFromDevice(const void* address, size_t length) const noexcept {
    mgr_.FetchRange(address, length);
}

size_t DMAMemoryImpl::TotalSize() const noexcept {
    return mgr_.TotalSize();
}

size_t DMAMemoryImpl::AvailableSize() const noexcept {
    return mgr_.AvailableSize();
}

} // namespace ASFW::Async
