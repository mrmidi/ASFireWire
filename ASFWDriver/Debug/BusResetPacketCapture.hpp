#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>

namespace ASFW::Debug {

/// Maximum number of bus reset packets to retain in circular buffer
constexpr size_t kBusResetPacketHistorySize = 32;

/// Snapshot of a single bus reset packet for debugging
///
/// Captures both the raw DMA format (little-endian) and the wire format (big-endian)
/// to assist with debugging endianness issues and packet parsing.
struct BusResetPacketSnapshot {
    /// Timestamp when packet was captured (mach_absolute_time)
    uint64_t captureTimestamp{0};

    /// Generation number from packet Q1[31:24] (wire format)
    uint32_t generation{0};

    /// Event code from trailer (should always be 0x09 for bus reset)
    uint8_t eventCode{0};

    /// tCode from Q0[31:28] (should always be 0xE for PHY packet)
    uint8_t tCode{0};

    /// Cycle time from trailer timestamp field
    uint16_t cycleTime{0};

    /// Raw quadlets as read from DMA buffer (little-endian)
    uint32_t rawQuadlets[4]{};

    /// Quadlets converted to wire format (big-endian)
    uint32_t wireQuadlets[4]{};

    /// Context string describing when/why packet was captured
    char contextInfo[64]{};

    /// Default constructor
    BusResetPacketSnapshot() = default;

    /// Copy constructor
    BusResetPacketSnapshot(const BusResetPacketSnapshot& other) {
        captureTimestamp = other.captureTimestamp;
        generation = other.generation;
        eventCode = other.eventCode;
        tCode = other.tCode;
        cycleTime = other.cycleTime;
        std::memcpy(rawQuadlets, other.rawQuadlets, sizeof(rawQuadlets));
        std::memcpy(wireQuadlets, other.wireQuadlets, sizeof(wireQuadlets));
        std::memcpy(contextInfo, other.contextInfo, sizeof(contextInfo));
    }

    /// Copy assignment
    BusResetPacketSnapshot& operator=(const BusResetPacketSnapshot& other) {
        if (this != &other) {
            captureTimestamp = other.captureTimestamp;
            generation = other.generation;
            eventCode = other.eventCode;
            tCode = other.tCode;
            cycleTime = other.cycleTime;
            std::memcpy(rawQuadlets, other.rawQuadlets, sizeof(rawQuadlets));
            std::memcpy(wireQuadlets, other.wireQuadlets, sizeof(wireQuadlets));
            std::memcpy(contextInfo, other.contextInfo, sizeof(contextInfo));
        }
        return *this;
    }
};

/// Circular buffer for capturing bus reset packet history
///
/// Thread-safe for single writer, multiple readers.
/// Writer advances atomically, readers access completed entries.
///
/// Usage:
///   BusResetPacketCapture capture;
///   capture.CapturePacket(dmaQuadlets, generation, "Initial reset");
///
///   // Later, retrieve for GUI
///   size_t count = capture.GetCount();
///   for (size_t i = 0; i < count; ++i) {
///       auto* snapshot = capture.GetSnapshot(i);
///       // Process snapshot...
///   }
class BusResetPacketCapture {
public:
    BusResetPacketCapture();
    ~BusResetPacketCapture();

    /// Capture a bus reset packet
    ///
    /// @param dmaQuadlets Pointer to 4 quadlets from DMA buffer (little-endian)
    /// @param generation Generation number from packet (for validation)
    /// @param context Optional context string (truncated to 63 chars)
    void CapturePacket(const uint32_t* dmaQuadlets,
                      uint8_t generation,
                      const char* context = nullptr);

    /// Get a snapshot by index (0 = oldest, count-1 = newest)
    ///
    /// @param index Index into history (0 to GetCount()-1)
    /// @return Pointer to snapshot, or nullptr if index out of range
    const BusResetPacketSnapshot* GetSnapshot(size_t index) const;

    /// Get total number of packets captured (max 32)
    ///
    /// @return Number of valid snapshots available
    size_t GetCount() const;

    /// Clear all captured packets
    void Clear();

    /// Get the most recent snapshot
    ///
    /// @return Pointer to newest snapshot, or nullptr if none captured
    const BusResetPacketSnapshot* GetLatest() const;

private:
    /// Circular buffer of snapshots
    std::array<BusResetPacketSnapshot, kBusResetPacketHistorySize> ring_;

    /// Next write index (wraps at kBusResetPacketHistorySize)
    std::atomic<uint32_t> writeIndex_{0};

    /// Total packets captured (saturates at kBusResetPacketHistorySize)
    std::atomic<uint32_t> count_{0};
};

} // namespace ASFW::Debug
