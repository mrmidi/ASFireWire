#pragma once

namespace ASFW::Discovery {

class ROMScannerCompletionManager {
public:
    void Reset() noexcept {
        notified_ = false;
    }

    void MarkNotified() noexcept {
        notified_ = true;
    }

    [[nodiscard]] bool TryMarkNotified() noexcept {
        if (notified_) {
            return false;
        }
        notified_ = true;
        return true;
    }

    [[nodiscard]] bool IsNotified() const noexcept {
        return notified_;
    }

private:
    bool notified_{false};
};

} // namespace ASFW::Discovery
