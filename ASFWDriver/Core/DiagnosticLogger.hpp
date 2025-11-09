#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <optional>

#include "RegisterMap.hpp"

namespace ASFW::Driver {

// Diagnostic logging helpers adapted from Linux firewire-ohci.c
// Provides detailed decode of interrupt events, Self-ID packets, and async packets
// for debugging complex OHCI timing and DMA issues.
class DiagnosticLogger {
public:
    // Log interrupt event register with decoded bit names
    // Adapted from Linux log_irqs() (ohci.c)
    static std::string DecodeInterruptEvents(uint32_t events);

    // Pretty-print Self-ID packet sequence with port status, speed, power
    // Adapted from Linux log_selfids() (ohci.c)
    static std::string DecodeSelfIDSequence(std::span<const uint32_t> selfIdBuffer,
                                           uint32_t generation,
                                           uint32_t nodeId);

    // Decode async transmit/receive packet header (AR/AT contexts)
    // Adapted from Linux log_ar_at_event() (ohci.c)
    enum class Direction : char { Receive = 'R', Transmit = 'T' };
    static std::string DecodeAsyncPacket(Direction dir,
                                        uint32_t speed,
                                        std::span<const uint32_t> header,
                                        uint32_t evt);

    // Decode OHCI event codes (DMA descriptor completion events)
    // Adapted from Linux evts[] table (ohci.c lines 508-525)
    static std::string DecodeEventCode(uint8_t eventCode);

    // Decode PHY packet (for link-internal packets)
    // Adapted from Linux PHY packet handling
    static std::string DecodePhyPacket(uint32_t phy0, uint32_t phy1);

private:
    // Transaction codes (IEEE 1394 §6.2)
    enum class TCode : uint8_t {
        WriteQuadletRequest = 0,
        WriteBlockRequest = 1,
        WriteResponse = 2,
        ReadQuadletRequest = 4,
        ReadBlockRequest = 5,
        ReadQuadletResponse = 6,
        ReadBlockResponse = 7,
        CycleStart = 8,
        LockRequest = 9,
        StreamData = 10,
        LockResponse = 11,
        LinkInternal = 14,
    };

    // PHY packet port status (IEEE 1394a §4.3.4.1)
    enum class PortStatus : uint8_t {
        None = 0,        // No device connected
        NotConnected = 1, // Device not connected
        Parent = 2,      // Parent port (upstream)
        Child = 3,       // Child port (downstream)
    };

    static constexpr std::array<std::string_view, 16> kTCodeNames = {
        "QW req",      // 0: Quadlet Write Request
        "BW req",      // 1: Block Write Request
        "W resp",      // 2: Write Response
        "-reserved-",  // 3
        "QR req",      // 4: Quadlet Read Request
        "BR req",      // 5: Block Read Request
        "QR resp",     // 6: Quadlet Read Response
        "BR resp",     // 7: Block Read Response
        "cycle start", // 8
        "Lk req",      // 9: Lock Request
        "async stream",// 10: Async Stream Packet
        "Lk resp",     // 11: Lock Response
        "-reserved-",  // 12
        "-reserved-",  // 13
        "link internal",// 14
        "-reserved-",  // 15
    };

    static constexpr std::array<std::string_view, 4> kSpeedNames = {
        "S100", "S200", "S400", "beta"
    };

    static constexpr std::array<std::string_view, 8> kPowerNames = {
        "+0W", "+15W", "+30W", "+45W",
        "-3W", " ?W", "-3..-6W", "-3..-10W"
    };

    static constexpr std::array<char, 4> kPortChars = {
        '.', '-', 'p', 'c'  // None, NotConnected, Parent, Child
    };

    // Extract tcode from packet header
    static constexpr TCode GetTCode(uint32_t header0) {
        return static_cast<TCode>((header0 >> 4) & 0xF);
    }

    // Extract fields from async packet header (big-endian wire format)
    static constexpr uint32_t GetDestination(uint32_t header0) {
        return (header0 >> 16) & 0xFFFF;
    }

    static constexpr uint32_t GetTLabel(uint32_t header0) {
        return (header0 >> 10) & 0x3F;
    }

    static constexpr uint32_t GetSource(uint32_t header1) {
        return (header1 >> 16) & 0xFFFF;
    }

    static constexpr uint64_t GetOffset(uint32_t header1, uint32_t header2) {
        return (static_cast<uint64_t>(header1 & 0xFFFF) << 32) | header2;
    }

    static constexpr uint32_t GetDataLength(uint32_t header3) {
        return (header3 >> 16) & 0xFFFF;
    }

    static constexpr uint32_t GetExtendedTCode(uint32_t header3) {
        return header3 & 0xFFFF;
    }

    // Extract PHY ID from Self-ID packet
    static constexpr uint32_t GetPhyId(uint32_t selfId) {
        return (selfId >> 24) & 0x3F;
    }

    // Extract port status from Self-ID sequence
    static PortStatus GetPortStatus(std::span<const uint32_t> sequence, size_t portIndex);
};

} // namespace ASFW::Driver
