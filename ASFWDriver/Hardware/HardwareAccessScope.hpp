#pragma once

#include <cstdint>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#endif

#include "RegisterMap.hpp"

namespace ASFW::Driver {

class HardwareInterface;

/// A stack-bound lease for one logical OHCI MMIO batch.
///
/// A scope is move-only and must not be captured, stored, dispatched to another
/// queue, or held while waiting.  Revocation closes the gate and waits for every
/// live scope to exit before the PCI provider can be released.
class HardwareAccessScope {
  public:
    HardwareAccessScope() = default;
    ~HardwareAccessScope();

    HardwareAccessScope(const HardwareAccessScope&) = delete;
    HardwareAccessScope& operator=(const HardwareAccessScope&) = delete;
    HardwareAccessScope(HardwareAccessScope&& other) noexcept;
    HardwareAccessScope& operator=(HardwareAccessScope&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return hardware_ != nullptr; }

    [[nodiscard]] uint32_t Read(Register32 reg) const noexcept;
    void Write(Register32 reg, uint32_t value) const noexcept;
    void WriteAndFlush(Register32 reg, uint32_t value) const noexcept;
    void FlushPostedWrites() const noexcept;

  private:
    friend class HardwareAccessGate;
    HardwareAccessScope(HardwareInterface* hardware, IOLock* lock) noexcept
        : hardware_(hardware), lock_(lock) {}

    void Release() noexcept;

    HardwareInterface* hardware_{nullptr};
    IOLock* lock_{nullptr};
};

} // namespace ASFW::Driver
