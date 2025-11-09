#include "BarrierUtils.hpp"

#include <atomic>

namespace ASFW::Driver {

void WriteBarrier() {
    std::atomic_thread_fence(std::memory_order_release);
}

void ReadBarrier() {
    std::atomic_thread_fence(std::memory_order_acquire);
}

void FullBarrier() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

} // namespace ASFW::Driver

