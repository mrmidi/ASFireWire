#pragma once

#include <cstdint>
#include <functional>

#include "../Async/AsyncTypes.hpp"
#include "DiscoveryTypes.hpp"

namespace ASFW::Async {
class AsyncSubsystem;
}

namespace ASFW::Discovery {

// High-level wrapper around AsyncSubsystem for Config ROM reads.
// Provides convenient helpers for reading Bus Info Block (BIB) and
// root directory quadlets with generation and speed tracking.
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

    explicit ROMReader(Async::AsyncSubsystem& asyncSubsystem);
    ~ROMReader() = default;

    // Read Bus Info Block (16 bytes, 4 quadlets) at standard Config ROM address
    // Address: 0xFFFFF0000400 (IEEE 1394-1995 §8.3.2)
    // Callback invoked on completion with result (success or failure)
    // busBase16: (bus << 6) from TopologySnapshot, used to compose full destinationID
    void ReadBIB(uint8_t nodeId,
                 Generation generation,
                 FwSpeed speed,
                 uint16_t busBase16,
                 CompletionCallback callback);

    // Read N quadlets from root directory starting at given offset
    // Offset is relative to BIB start (0xFFFFF0000400)
    // Typical usage: offset=16 (skip BIB), count=8-16 (bounded scan)
    // busBase16: (bus << 6) from TopologySnapshot, used to compose full destinationID
    void ReadRootDirQuadlets(uint8_t nodeId,
                             Generation generation,
                             FwSpeed speed,
                             uint16_t busBase16,
                             uint32_t offsetBytes,
                             uint32_t count,
                             CompletionCallback callback);

private:
    // Convert FwSpeed enum to OHCI speed code (0=S100, 1=S200, 2=S400, 3=S800)
    static uint8_t SpeedToCode(FwSpeed speed);

    Async::AsyncSubsystem& async_;
};

} // namespace ASFW::Discovery

