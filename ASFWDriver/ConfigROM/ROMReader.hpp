#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ConfigROMConstants.hpp"

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
    struct BIBReadContext {
        CompletionCallback userCallback;
        uint8_t nodeId{0};
        Generation generation{0};
        std::vector<uint32_t> buffer;
        uint8_t quadletIndex{0};
        uint8_t successCount{0};
    };

    struct RootDirReadContext {
        CompletionCallback userCallback;
        uint8_t nodeId{0};
        Generation generation{0};
        uint32_t baseAddress{0};
        uint32_t quadletCount{0};
        std::vector<uint32_t> buffer;
        uint32_t quadletIndex{0};
        uint32_t successCount{0};
        bool headerFirstMode{false};
    };

    static constexpr uint32_t kBIBLength = ASFW::ConfigROM::kBIBLengthBytes;
    static constexpr uint32_t kBIBQuadlets = ASFW::ConfigROM::kBIBQuadletCount;

    Async::IFireWireBus& bus_;
    OSSharedPtr<IODispatchQueue> dispatchQueue_;

    void ScheduleBIBStep(const std::shared_ptr<BIBReadContext>& ctx);
    void HandleBIBReadComplete(const std::shared_ptr<BIBReadContext>& ctx,
                               Async::AsyncStatus status,
                               std::span<const uint8_t> responsePayload);
    void EmitBIBResult(const std::shared_ptr<BIBReadContext>& ctx,
                       bool success) const;

    void ScheduleRootDirStep(const std::shared_ptr<RootDirReadContext>& ctx);
    void HandleRootDirReadComplete(const std::shared_ptr<RootDirReadContext>& ctx,
                                   Async::AsyncStatus status,
                                   std::span<const uint8_t> responsePayload);
    void EmitRootDirFailure(const std::shared_ptr<RootDirReadContext>& ctx) const;
    void EmitRootDirResult(const std::shared_ptr<RootDirReadContext>& ctx,
                           bool success,
                           uint32_t quadletCountForResult) const;

    void ScheduleNextQuadlet(std::function<void()> task);
};

} // namespace ASFW::Discovery

inline void ASFW::Discovery::ROMReader::ScheduleNextQuadlet(std::function<void()> task) {
    if (!dispatchQueue_) {
#ifdef ASFW_HOST_TEST
        // In host tests without a dispatch queue, we must NOT call issueNextQuadlet
        // synchronously because it leads to deep recursion and use-after-free
        // of the context during stack unwinding.
        std::thread([task = std::move(task)]() mutable {
            task();
        }).detach();
#else
        task();
#endif
        return;
    }

    auto queue = dispatchQueue_;
    auto capturedTask = std::move(task);
    queue->DispatchAsync(^{
        capturedTask();
    });
}
