// UncachedFill.hpp
// ASFW - filling cache-inhibited DMA mappings.
//
// DMA slabs are mapped kIOMemoryMapCacheModeInhibit, so CPU stores reach RAM
// without a flush. That memory does not accept cache-maintenance instructions,
// and `dc zva` is one. Apple's __bzero picks `dc zva` by block size, so
// memset() over such a mapping works below a libc-internal size and faults
// above it -- reported as EXC_ARM_DA_ALIGN at a correctly aligned address.
// Found on the midi branch (51a150d6, 2026-09-07) when the IT descriptor slab
// grew from 4 KiB to 32 KiB. Size is not the safety argument; the store
// instruction is.
//
// Use this for any fill wider than one descriptor. A memset over a single
// 16-byte OHCIDescriptor is plain stores at any size libc supports.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::Shared {

/// Byte-fill a cache-inhibited DMA region with plain volatile stores.
inline void FillUncachedDma(void* base, uint8_t value, size_t length) noexcept {
    if (base == nullptr || length == 0) {
        return;
    }
    auto* bytes = static_cast<volatile uint8_t*>(base);
    for (size_t i = 0; i < length; ++i) {
        bytes[i] = value;
    }
}

} // namespace ASFW::Shared
