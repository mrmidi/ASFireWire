#pragma once

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#endif

#include "HardwareAccessScope.hpp"

namespace ASFW::Driver {

class HardwareInterface;

/// Serializes local OHCI access with provider revocation.
class HardwareAccessGate {
  public:
    HardwareAccessGate() : lock_(IOLockAlloc()) {}
    ~HardwareAccessGate() {
        if (lock_) {
            IOLockFree(lock_);
        }
    }

    HardwareAccessGate(const HardwareAccessGate&) = delete;
    HardwareAccessGate& operator=(const HardwareAccessGate&) = delete;

    [[nodiscard]] HardwareAccessScope TryBeginAccess(HardwareInterface& hardware) noexcept {
        if (!lock_) {
            return {};
        }
        IOLockLock(lock_);
        if (!open_) {
            IOLockUnlock(lock_);
            return {};
        }
        return HardwareAccessScope(&hardware, lock_);
    }

    /// Prevent new MMIO scopes and wait for the active batch, if any, to end.
    void RevokeAndDrain() noexcept {
        if (!lock_) {
            return;
        }
        IOLockLock(lock_);
        open_ = false;
        IOLockUnlock(lock_);
    }

    /// Opens access only after the new provider is fully initialized.
    void Open() noexcept {
        if (!lock_) {
            return;
        }
        IOLockLock(lock_);
        open_ = true;
        IOLockUnlock(lock_);
    }

    [[nodiscard]] bool IsOpen() const noexcept {
        if (!lock_) {
            return false;
        }
        IOLockLock(lock_);
        const bool open = open_;
        IOLockUnlock(lock_);
        return open;
    }

  private:
    IOLock* lock_{nullptr};
    bool open_{false};
};

} // namespace ASFW::Driver
