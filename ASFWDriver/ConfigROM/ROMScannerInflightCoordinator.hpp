#pragma once

#include <cstdint>

namespace ASFW::Discovery {

class ROMScannerInflightCoordinator {
public:
    void Reset() noexcept { count_ = 0; }

    void Increment() noexcept {
        ++count_;
    }

    void Decrement() noexcept {
        if (count_ > 0) {
            --count_;
        }
    }

    [[nodiscard]] bool HasCapacity(uint16_t maxInflight) const noexcept {
        return count_ < maxInflight;
    }

    [[nodiscard]] uint16_t Count() const noexcept {
        return count_;
    }

private:
    uint16_t count_{0};
};

} // namespace ASFW::Discovery
