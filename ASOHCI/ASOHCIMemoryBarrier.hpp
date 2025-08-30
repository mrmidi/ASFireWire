#pragma once
//
// ASOHCIMemoryBarrier.hpp
// DriverKit-compatible memory barriers for OHCI DMA descriptor synchronization
//
// OHCI Spec Requirements:
//   §7.1, §8.1, §9.1: DMA descriptor chains must be coherent before hardware
//   access §7.4, §8.4, §9.4: Safe program appending requires memory ordering
//   guarantees Hardware must see: Descriptor writes → CommandPtr update →
//   Wake/Run operation

#include <atomic>

namespace ASOHCIBarrier {

// Full memory barrier for critical OHCI synchronization points
// Ensures all prior memory operations complete before subsequent operations
// begin Maps to OHCI requirements for descriptor→CommandPtr→hardware visibility
inline void FullFence() { std::atomic_thread_fence(std::memory_order_seq_cst); }

// Release barrier for descriptor write completion
// Ensures all descriptor writes are visible before CommandPtr/hardware updates
// Used before CommandPtr writes and hardware wake operations
inline void ReleaseFence() {
  std::atomic_thread_fence(std::memory_order_release);
}

// Acquire barrier for hardware state reads
// Ensures hardware state reads are not reordered with subsequent operations
// Used after reading context status before making decisions
inline void AcquireFence() {
  std::atomic_thread_fence(std::memory_order_acquire);
}

} // namespace ASOHCIBarrier

// Primary barrier for OHCI descriptor synchronization
// Use this at critical points where hardware must see consistent descriptor
// state
#define OHCI_MEMORY_BARRIER() ASOHCIBarrier::FullFence()