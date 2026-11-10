#pragma once

#include <cstddef>
#include <cstdint>
#include <DriverKit/IOLib.h> // OSSynchronizeIO
#include "../Shared/Memory/DMAMemoryManager.hpp"

namespace ASFW::Driver {

// DriverKit-friendly memory barrier helpers. Implemented using C++ atomics
// for normal memory ordering and OSSynchronizeIO() for MMIO ordering.
void WriteBarrier();   // publish normal-memory writes (release)
void ReadBarrier();    // consume normal-memory reads (acquire)
void FullBarrier();    // full fence for rare cases

// MMIO barrier for programming device registers
inline void IoBarrier() { OSSynchronizeIO(); }

// DriverKit 27 IOLib.h:235-239 documents OSSynchronizeIO as DMB ST on
// Apple Silicon (also verified in the installed DriverKit library). It does
// not order reads. RX needs completion -> payload reads -> status rearm.
// Use the full system domain for device DMA, not a C++ inner-shareable fence.
// This orders accesses; it is not cache maintenance or a DMA completion wait.
inline void DmaOwnershipBarrier() {
#if defined(__aarch64__)
    __builtin_arm_dmb(0xf); // DMB SY: all loads/stores, full system domain.
#else
    OSSynchronizeIO(); // DriverKit x86_64: MFENCE.
#endif
}

// Convenience wrappers for raw MMIO 32-bit accesses
inline void Write32(volatile uint32_t* addr, uint32_t value) {
    *addr = value;
    IoBarrier();
}

inline uint32_t Read32(volatile uint32_t* addr) {
    IoBarrier();
    return *addr;
}

} // namespace ASFW::Driver

namespace ASFW::Driver {

// Consistent DMA publish helper: synchronize DMA range and ensure MMIO ordering
// Use this after building/patching any descriptor you expect the HC to fetch,
// after setting control word, and prior to WAKE or programming CommandPtr+RUN
inline void PublishForDMA(ASFW::Shared::DMAMemoryManager& mm, const std::byte* p, size_t n) {
    mm.PublishRange(p, n);
    IoBarrier();
}

template <typename T>
inline void PublishForDMA(ASFW::Shared::DMAMemoryManager& mm, const T* p, size_t n) {
    PublishForDMA(mm, reinterpret_cast<const std::byte*>(p), n);
}

} // namespace ASFW::Driver
