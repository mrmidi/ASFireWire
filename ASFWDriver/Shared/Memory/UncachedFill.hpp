// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Filling cache-inhibited DMA mappings.
//
// DMA slabs are mapped kIOMemoryMapCacheModeInhibit so CPU writes reach RAM
// without a flush. Device-nGnRnE memory does not accept cache-maintenance
// instructions, and `dc zva` is one: issuing it raises a data abort that
// arrives as EXC_ARM_DA_ALIGN / SIGBUS, at a *correctly aligned* address, which
// makes it read like anything but what it is.
//
// Apple's __bzero selects `dc zva` by block size, so memset() on such a mapping
// is not wrong-or-right, it is wrong-above-a-threshold that is a libc
// implementation detail. IsochTxDescriptorSlab called memset on its descriptor
// region for years without incident at 4 KiB and crashed on the first start
// after the TX ring deepened it to 32 KiB (2026-09-07,
// IsochTxDescriptorSlab::AllocateAndInitialize -> __bzero). Size is not a
// safety argument here; the store instruction is.
//
// Use this for anything wider than a single descriptor. The small fixed-size
// memsets over one OHCIDescriptor are plain stores at any libc and are left
// alone.

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
