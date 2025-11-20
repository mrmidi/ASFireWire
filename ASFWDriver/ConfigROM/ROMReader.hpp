#pragma once

#include <cstdint>
#include <functional>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../Async/AsyncTypes.hpp"
#include "../Discovery/DiscoveryTypes.hpp"

namespace ASFW::Async {
class IFireWireBus;
}

namespace ASFW::Discovery {

// High-level wrapper around IFireWireBus for Config ROM reads.
// Provides convenient helpers for reading Bus Info Block (BIB) and
// root directory quadlets with generation and speed tracking.
//
// Phase 2: Refactored to use IFireWireBus interface instead of AsyncSubsystem.
// Only supports quadlet-mode reads (block reads for Config ROM are problematic).
class ROMReader {
public:
    // Result passed to completion callbacks
    struct ReadResult {
        bool success{false};
        uint8_t nodeId{0xFF};
        Generation generation{0};
        uint32_t address{0};
        const uint32_t* data{nullptr};  // Points to caller-provided buffer
        uint32_t dataLength{0};         // Length in bytes
    };

    using CompletionCallback = std::function<void(const ReadResult&)>;

    explicit ROMReader(Async::IFireWireBus& bus,
                       OSSharedPtr<IODispatchQueue> dispatchQueue = nullptr);
    ~ROMReader() = default;

    // Read Bus Info Block (20 bytes, 5 quadlets) at standard Config ROM address
    // Address: 0xFFFFF0000400 (IEEE 1394-1995 §8.3.2)
    // Callback invoked on completion with result (success or failure)
    // Note: Always uses S100 speed for Config ROM reads (per Apple behavior)
    void ReadBIB(uint8_t nodeId,
                 Generation generation,
                 FwSpeed speed,
                 CompletionCallback callback);

    // Read N quadlets from root directory starting at given offset
    // Offset is relative to BIB start (0xFFFFF0000400)
    // Typical usage: offset=20 (skip BIB), count=8-16 (bounded scan)
    // Note: Always uses S100 speed for Config ROM reads (per Apple behavior)
    void ReadRootDirQuadlets(uint8_t nodeId,
                             Generation generation,
                             FwSpeed speed,
                             uint32_t offsetBytes,
                             uint32_t count,
                             CompletionCallback callback);

private:
    Async::IFireWireBus& bus_;
    OSSharedPtr<IODispatchQueue> dispatchQueue_;

    template <typename Context>
    void ScheduleNextQuadlet(Context* ctx);
};

} // namespace ASFW::Discovery

template <typename Context>
inline void ASFW::Discovery::ROMReader::ScheduleNextQuadlet(Context* ctx) {
    if (!dispatchQueue_) {
        ctx->issueNextQuadlet();
        return;
    }

    auto queue = dispatchQueue_;
    Context* capturedCtx = ctx;
    queue->DispatchAsync(^{
        capturedCtx->issueNextQuadlet();
    });
}
