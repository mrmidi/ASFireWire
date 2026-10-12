// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::FW {

// IEEE 1394-2008 8.3.2.2.6: seconds + cycles in units of 125 us;
// effective minimum 800 cycles. Preserve the CSR value even below that minimum.
// Compatibility default matches Linux core-card.c:83-87 (slow device firmware).
// One controller-owned policy is shared by CSR, request tracking and response TX.
class SplitTimeout {
  public:
    [[nodiscard]] uint32_t Hi() const noexcept { return hi_.load(); }
    [[nodiscard]] uint32_t Lo() const noexcept { return lo_.load(); }
    void WriteHi(uint32_t value) noexcept { hi_.store(value & 7u); }
    void WriteLo(uint32_t value) noexcept { lo_.store(value & 0xfff80000u); }
    [[nodiscard]] uint32_t Cycles() const noexcept {
        const auto cycles = Hi() * 8000u + (Lo() >> 19);
        // Linux core-transaction.c:1141-1151 bounds effective timeout to
        // three seconds so OHCI response expiry cannot overflow its window.
        return cycles < 800u ? 800u : cycles > 24000u ? 24000u : cycles;
    }
    [[nodiscard]] uint64_t Microseconds() const noexcept { return uint64_t{Cycles()} * 125u; }
    [[nodiscard]] uint16_t ResponseDeadline(uint16_t receivedStamp) const noexcept {
        const auto cycles = Cycles() + (receivedStamp & 0x1fffu);
        const auto seconds = (receivedStamp >> 13) + cycles / 8000u;
        return static_cast<uint16_t>((seconds << 13) | (cycles % 8000u));
    }

    // OHCI AT timeStamp is seconds modulo 8 + cycle count. Account for completion
    // drain latency using the current cycle timer. Keep one cycle of quantization
    // allowance: the descriptor does not record the sub-cycle offset.
    [[nodiscard]] static uint64_t RequestTimeUs(uint16_t sentStamp, uint32_t cycleTimer,
                                                uint64_t nowUs) noexcept {
        const auto sentCycle = (sentStamp >> 13) * 8000u + (sentStamp & 0x1fffu);
        const auto nowCycle = ((cycleTimer >> 25) & 7u) * 8000u + ((cycleTimer >> 12) & 0x1fffu);
        const auto ageCycles = (nowCycle + 64000u - sentCycle) % 64000u;
        const auto ageUs = ageCycles > 0 ? uint64_t{ageCycles - 1u} * 125u : 0u;
        return nowUs > ageUs ? nowUs - ageUs : 0u;
    }

  private:
    std::atomic<uint32_t> hi_{2};
    std::atomic<uint32_t> lo_{0};
};

} // namespace ASFW::FW
