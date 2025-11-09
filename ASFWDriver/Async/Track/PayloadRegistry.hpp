#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <DriverKit/IOLib.h>

namespace ASFW::Async {

class PayloadRegistry {
public:
    enum class CancelMode {
        Deferred,
        Synchronous
    };

    PayloadRegistry();
    ~PayloadRegistry();

    // Attach a payload for a given outstanding handle. The registry takes
    // ownership via shared_ptr so callers can pass ownership-friendly types.
    void Attach(uint32_t handle, std::shared_ptr<void> payload, uint32_t epoch = 0);

    // Detach and return the payload for the given handle (or nullptr if none).
    std::shared_ptr<void> Detach(uint32_t handle);

    // Cancel all payloads. If mode==Synchronous, blocks until drain completes.
    void CancelAll(CancelMode mode = CancelMode::Deferred);

    // Cancel payloads with epoch <= given epoch.
    void CancelByEpoch(uint32_t epoch, CancelMode mode = CancelMode::Deferred);

    // Drain waits for registry to become empty or until timeoutMs elapses.
    // Returns true if empty, false on timeout.
    bool Drain(uint32_t timeoutMs);

    // Utility to set/get epoch counter used by callers.
    void SetEpoch(uint32_t epoch);
    uint32_t GetEpoch() const;

private:
    struct Entry {
        std::shared_ptr<void> payload;
        uint32_t epoch{0};
    };

    ::IOLock* lock_{nullptr};
    std::unordered_map<uint32_t, Entry> map_;
    uint32_t epoch_{0};
};

} // namespace ASFW::Async
