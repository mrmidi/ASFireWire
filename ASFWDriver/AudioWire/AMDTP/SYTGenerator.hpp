//
// SYTGenerator.hpp
// ASFWDriver
//
// 48 kHz TX SYT generation for blocking-mode playback.
// Seeds from established RX SYT phase, then advances by a fixed per-DATA-packet
// step to match the sample-domain behavior seen in Saffire.kext captures.
//

#pragma once

#include <cstdint>

namespace ASFW::Encoding {

/// Generates 48 kHz blocking-mode SYT timestamps from an RX-seeded sample timeline.
///
/// The current milestone intentionally keeps hardware transmit-cycle tracking out of
/// the emitted SYT values. Local OHCI time is still used by the DMA ring for packet
/// scheduling, but playback SYT itself is seeded from the last established RX SYT
/// and then advanced by a fixed 4096-tick step per DATA packet.
class SYTGenerator {
public:
    explicit SYTGenerator() noexcept = default;

    /// Initialize timing for given sample rate
    /// @param sampleRate Sample rate in Hz (e.g., 48000.0)
    void initialize(double sampleRate) noexcept;

    /// Reset running state (call on stream start)
    void reset() noexcept;

    /// Seed the 48 kHz packet-step timeline from the last established RX SYT.
    void seedFromRxSyt(uint16_t rxSyt) noexcept;

    /// Compute SYT for a DATA packet at the given OHCI transmit cycle
    /// @param transmitCycle 13-bit OHCI cycle count (0-7999)
    /// @param samplesInPacket Data blocks (events) carried in this DATA packet
    /// @return 16-bit SYT value
    [[nodiscard]] uint16_t computeDataSYT(uint32_t transmitCycle, uint32_t samplesInPacket) noexcept;

    /// Apply a small signed offset correction in 16-cycle tick domain.
    void nudgeOffsetTicks(int32_t deltaTicks) noexcept;

    /// SYT value meaning "no timestamp information"
    static constexpr uint16_t kNoInfo = 0xFFFF;

    /// Check if generator is initialized
    [[nodiscard]] bool isValid() const noexcept { return initialized_; }

    /// Get DATA packet counter for diagnostics
    [[nodiscard]] uint64_t dataPacketCount() const noexcept { return dataPacketCount_; }

    /// Current tick index in the 16-cycle SYT domain [0..49151].
    /// Used by the discipline loop to compare TX phase against RX.
    [[nodiscard]] uint32_t currentSytTickIndex() const noexcept { return currentSytTickIndex_; }

private:
    // =========================================================================
    // Timing constants
    // =========================================================================

    /// 24.576 MHz ticks per 125 us bus cycle
    static constexpr uint32_t kTicksPerCycle = 3072;

    /// Ticks per audio sample at 48 kHz: 24576000 / 48000 = 512
    static constexpr uint32_t kTicksPerSample48k = 512;

    // =========================================================================
    // Per-rate computed values (set in initialize())
    // =========================================================================

    /// Ticks per sample at the active sample rate. For 48 kHz: 512.
    uint32_t ticksPerSample_{kTicksPerSample48k};

    /// Fixed 48 kHz blocking-mode step: 8 samples * 512 ticks/sample = 4096.
    uint32_t packetStepTicks_{8 * kTicksPerSample48k};

    /// Wrap point for the 16-cycle SYT domain.
    uint32_t sytTickWrap_{16 * kTicksPerCycle};  // 49152

    // =========================================================================
    // Running state
    // =========================================================================

    /// Current SYT tick index in the 16-cycle domain [0..49151].
    uint32_t currentSytTickIndex_{0};

    /// Whether the generator has been seeded from an RX SYT.
    bool seeded_{false};

    /// Diagnostic counter
    uint64_t dataPacketCount_{0};

    /// Whether initialize() has been called
    bool initialized_{false};
};

} // namespace ASFW::Encoding
