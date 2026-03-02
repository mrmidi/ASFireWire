#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "Common/ConfigROMConstants.hpp"

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
    // Result passed to completion callbacks.
    struct ReadResult {
        bool success{false};
        uint8_t nodeId{0xFF};
        Generation generation{0};
        uint32_t address{0}; // AddressLo (0xF0000400 + offsetBytes)
        Async::AsyncStatus status{Async::AsyncStatus::kHardwareError};

        // Quadlets are stored as raw wire-order big-endian bytes in host memory.
        // Consumers must byteswap (e.g. OSSwapBigToHostInt32) before interpreting fields.
        std::vector<uint32_t> quadletsBE;

        [[nodiscard]] std::span<const uint32_t> QuadletsBE() const noexcept {
            return {quadletsBE.data(), quadletsBE.size()};
        }

        [[nodiscard]] uint32_t DataLengthBytes() const noexcept {
            return static_cast<uint32_t>(quadletsBE.size() * sizeof(uint32_t));
        }
    };

    using CompletionCallback = std::function<void(ReadResult)>;

    enum class QuadletReadPolicy : uint8_t {
        // Any read error fails the operation.
        AllOrNothing,
        // After at least one successful quadlet, any read error is treated as
        // end-of-data and the operation completes successfully with a shortened prefix.
        AllowPartialEOF,
    };

    explicit ROMReader(Async::IFireWireBus& bus,
                       OSSharedPtr<IODispatchQueue> dispatchQueue = nullptr);
    ~ROMReader() = default;

    // Primitive: read N quadlets from Config ROM address space.
    // Offset is relative to Config ROM base (0xFFFFF0000400).
    void ReadQuadletsBE(uint8_t nodeId, Generation generation, FwSpeed speed, uint32_t offsetBytes,
                        uint32_t quadletCount, CompletionCallback callback,
                        QuadletReadPolicy policy = QuadletReadPolicy::AllOrNothing);

    // Read Bus Info Block (20 bytes, 5 quadlets) at standard Config ROM address
    // Address: 0xFFFFF0000400 (IEEE 1394-1995 §8.3.2)
    // Callback invoked on completion with result (success or failure)
    void ReadBIB(uint8_t nodeId, Generation generation, FwSpeed speed, CompletionCallback callback);

    // Read N quadlets from root directory starting at given offset
    // Offset is relative to BIB start (0xFFFFF0000400)
    // Typical usage: offset=20 (skip BIB), count=8-16 (bounded scan)
    void ReadRootDirQuadlets(uint8_t nodeId, Generation generation, FwSpeed speed,
                             uint32_t offsetBytes, uint32_t count, CompletionCallback callback);

  private:
    struct QuadletReadContext {
        CompletionCallback userCallback;
        Async::IFireWireBus* bus{nullptr};
        OSSharedPtr<IODispatchQueue> dispatchQueue;
        uint8_t nodeId{0};
        Generation generation{0};
        FwSpeed speed{FwSpeed::S100};
        uint32_t baseAddress{0};
        uint32_t quadletCount{0};
        QuadletReadPolicy policy{QuadletReadPolicy::AllOrNothing};
        std::vector<uint32_t> buffer;
        uint32_t quadletIndex{0};
        uint32_t successCount{0};
    };

    static constexpr uint32_t kBIBLength = ASFW::ConfigROM::kBIBLengthBytes;
    static constexpr uint32_t kBIBQuadlets = ASFW::ConfigROM::kBIBQuadletCount;

    Async::IFireWireBus& bus_;
    OSSharedPtr<IODispatchQueue> dispatchQueue_;

    static void ReadQuadletsBEImpl(Async::IFireWireBus& bus,
                                   OSSharedPtr<IODispatchQueue> dispatchQueue, uint8_t nodeId,
                                   Generation generation, FwSpeed speed, uint32_t offsetBytes,
                                   uint32_t quadletCount, CompletionCallback callback,
                                   QuadletReadPolicy policy);

    static void ScheduleQuadletReadStep(const std::shared_ptr<QuadletReadContext>& ctx);
    static void HandleQuadletReadComplete(const std::shared_ptr<QuadletReadContext>& ctx,
                                          Async::AsyncStatus status,
                                          std::span<const uint8_t> responsePayload);
    static void EmitQuadletReadResult(const std::shared_ptr<QuadletReadContext>& ctx, bool success,
                                      Async::AsyncStatus status, uint32_t quadletsToReturn);

    static void ScheduleNextQuadlet(OSSharedPtr<IODispatchQueue> dispatchQueue,
                                    std::function<void()> task);
};

} // namespace ASFW::Discovery
