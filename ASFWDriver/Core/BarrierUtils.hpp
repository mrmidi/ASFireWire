#pragma once

#include <cstdint>
#include <DriverKit/IOLib.h> // OSSynchronizeIO
#include "../Async/Core/DMAMemoryManager.hpp"

namespace ASFW::Driver {

// DriverKit-friendly memory barrier helpers. Implemented using C++ atomics
// for normal memory ordering and OSSynchronizeIO() for MMIO ordering.
void WriteBarrier();   // publish normal-memory writes (release)
void ReadBarrier();    // consume normal-memory reads (acquire)
void FullBarrier();    // full fence for rare cases

// MMIO barrier for programming device registers
inline void IoBarrier() { OSSynchronizeIO(); }

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

// Forward declaration for DMA memory manager
namespace ASFW::Async {
class DMAMemoryManager;
}

namespace ASFW::Driver {

// Consistent DMA publish helper: synchronize DMA range and ensure MMIO ordering
// Use this after building/patching any descriptor you expect the HC to fetch,
// after setting control word, and prior to WAKE or programming CommandPtr+RUN
inline void PublishForDMA(ASFW::Async::DMAMemoryManager& mm, const void* p, size_t n) {
    mm.PublishRange(p, n);
    IoBarrier();
}

} // namespace ASFW::Driver
