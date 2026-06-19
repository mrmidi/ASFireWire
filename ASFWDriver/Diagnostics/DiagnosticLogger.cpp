#include "DiagnosticLogger.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Use snprintf-based formatting to avoid depending on <sstream>/<iomanip>

namespace ASFW::Driver {

namespace {

struct InterruptBitName {
    uint32_t mask;
    std::string_view name;
};

constexpr std::array<InterruptBitName, 25> kInterruptBitNames = {{
    {IntEventBits::kReqTxComplete, "AT_req"},
    {IntEventBits::kRespTxComplete, "AT_resp"},
    {IntEventBits::kARRQ, "AR_req"},
    {IntEventBits::kARRS, "AR_resp"},
    {IntEventBits::kRQPkt, "RQPkt"},
    {IntEventBits::kRSPkt, "RSPkt"},
    {IntEventBits::kIsochTx, "IT"},
    {IntEventBits::kIsochRx, "IR"},
    {IntEventBits::kPostedWriteErr, "postedWriteErr"},
    {IntEventBits::kLockRespErr, "lockRespErr"},
    {IntEventBits::kSelfIDComplete2, "selfID2"},
    {IntEventBits::kSelfIDComplete, "selfID"},
    {IntEventBits::kBusReset, "busReset"},
    {IntEventBits::kRegAccessFail, "regAccessFail"},
    {IntEventBits::kPhy, "phy"},
    {IntEventBits::kCycleSynch, "cycleSynch"},
    {IntEventBits::kCycle64Seconds, "cycle64Seconds"},
    {IntEventBits::kCycleLost, "cycleLost"},
    {IntEventBits::kCycleInconsistent, "cycleInconsistent"},
    {IntEventBits::kUnrecoverableError, "unrecoverableError"},
    {IntEventBits::kCycleTooLong, "cycleTooLong"},
    {IntEventBits::kPhyRegRcvd, "phyRegRcvd"},
    {IntEventBits::kAckTardy, "ack_tardy"},
    {IntEventBits::kVendorSpecific, "vendor"},
    {IntMaskBits::kMasterIntEnable, "masterIntEnable"},
}};

constexpr uint32_t KnownInterruptBits() {
    uint32_t knownBits = 0;
    for (const auto& entry : kInterruptBitNames) {
        knownBits |= entry.mask;
    }
    return knownBits;
}

void AppendInterruptBitNames(std::string& out, const uint32_t events) {
    for (const auto& entry : kInterruptBitNames) {
        if (events & entry.mask) {
            out.push_back(' ');
            out += entry.name;
        }
    }
}

constexpr uint32_t kKnownInterruptBits = KnownInterruptBits();

} // namespace

std::string DiagnosticLogger::DecodeInterruptEvents(uint32_t events) {
    // Adapted from Linux log_irqs() (ohci.c lines 480-503)
    // Decodes interrupt event register into human-readable bit names
    // Per OHCI 1.1 Table 6-1 (IntEvent register description)
    std::string oss;
    char buf[64];
    snprintf(buf, sizeof(buf), "IRQ 0x%08x", events);
    oss += buf;

    AppendInterruptBitNames(oss, events);

    if (events & ~kKnownInterruptBits) {
        snprintf(buf, sizeof(buf), " UNKNOWN(0x%08x)", events & ~kKnownInterruptBits);
        oss += buf;
    }

    return oss;
}

std::string DiagnosticLogger::DecodeSelfIDSequence(std::span<const uint32_t> selfIdBuffer,
                                                   uint32_t generation,
                                                   uint32_t nodeId) {
    // Adapted from Linux log_selfids() (ohci.c lines 1940-2001)
    // Pretty-prints Self-ID packet sequences with port topology
    if (selfIdBuffer.empty()) {
        return "No Self-ID packets";
    }

    std::string oss;
    char buf[128];
    snprintf(buf, sizeof(buf), "%zu Self-ID quadlets, generation %u, local node ID 0x%04x\n",
             selfIdBuffer.size(), generation, nodeId);
    oss += buf;

    size_t idx = 0;
    while (idx < selfIdBuffer.size()) {
        const size_t quadletCount = GetSelfIDSequenceLength(selfIdBuffer, idx);
        const std::span<const uint32_t> sequence = selfIdBuffer.subspan(idx, quadletCount);
        AppendPrimarySelfID(oss, sequence);
        AppendExtendedSelfIDs(oss, sequence);
        idx += quadletCount;
    }

    return oss;
}

size_t DiagnosticLogger::GetSelfIDSequenceLength(const std::span<const uint32_t> selfIdBuffer,
                                                 const size_t start) noexcept {
    size_t quadletCount = 1;
    while (start + quadletCount < selfIdBuffer.size() &&
           (selfIdBuffer[start + quadletCount] & 0x80000000) == 0) {
        quadletCount++;
    }
    return quadletCount;
}

void DiagnosticLogger::AppendPortRange(std::string& out,
                                       const std::span<const uint32_t> sequence,
                                       const size_t firstPort,
                                       const size_t portCount) {
    const size_t lastPort = std::min<size_t>(27, firstPort + portCount);
    for (size_t portIndex = firstPort; portIndex < lastPort; ++portIndex) {
        out.push_back(kPortChars[static_cast<size_t>(GetPortStatus(sequence, portIndex))]);
    }
}

void DiagnosticLogger::AppendPrimarySelfID(std::string& out, const std::span<const uint32_t> sequence) {
    const uint32_t sid0 = sequence.front();
    const uint32_t phyId = GetPhyId(sid0);
    const uint32_t speed = (sid0 >> 14) & 0x3;
    const uint32_t gapCount = (sid0 >> 16) & 0x3F;
    const uint32_t powerClass = (sid0 >> 8) & 0x7;
    const bool linkActive = (sid0 >> 22) & 0x1;
    const bool contender = (sid0 >> 11) & 0x1;
    const bool initiator = (sid0 & 0x2) != 0;

    char buf[128];
    snprintf(buf, sizeof(buf), "  Self-ID PHY %u [", phyId);
    out += buf;
    AppendPortRange(out, sequence, 0, 3);
    out += "] ";
    out += std::string(kSpeedNames[speed]);
    snprintf(buf, sizeof(buf), " gc=%u ", gapCount);
    out += buf;
    out += std::string(kPowerNames[powerClass]);
    if (linkActive) {
        out += " L";
    }
    if (contender) {
        out += " c";
    }
    if (initiator) {
        out += " i";
    }
    out += "\n";
}

void DiagnosticLogger::AppendExtendedSelfIDs(std::string& out, const std::span<const uint32_t> sequence) {
    for (size_t quadletIndex = 1; quadletIndex < sequence.size(); ++quadletIndex) {
        out += "    Extended [";
        const size_t firstPort = 3 + (quadletIndex - 1) * 8;
        AppendPortRange(out, sequence, firstPort, 8);
        out += "]\n";
    }
}

std::string DiagnosticLogger::DecodeAsyncPacket(Direction dir,
                                               uint32_t speed,
                                               std::span<const uint32_t> header,
                                               uint32_t evt) {
    // Adapted from Linux log_ar_at_event() (ohci.c lines 526-609)
    // Decodes async receive/transmit packet headers
    if (header.empty()) {
        return "Invalid packet header (empty)";
    }

    const TCode tcode = GetTCode(header[0]);
    const size_t tcodeIdx = static_cast<size_t>(tcode);
    const std::string_view tcodeName = (tcodeIdx < kTCodeNames.size())
        ? kTCodeNames[tcodeIdx] : "INVALID";

    std::string oss;
    char buf[128];
    snprintf(buf, sizeof(buf), "A%c ", static_cast<char>(dir));
    oss += buf;

    // Special case: bus reset packet
    if (evt == 0x1F) {  // OHCI1394_evt_bus_reset
        if (header.size() >= 3) {
            const uint32_t gen = (header[2] >> 16) & 0xFF;
            snprintf(buf, sizeof(buf), "evt_bus_reset, generation %u", gen);
            oss += buf;
        } else {
            oss += "evt_bus_reset (incomplete header)";
        }
        return oss;
    }

    // Build tcode-specific details
    std::string specific;
    if (header.size() >= 4) {
        switch (tcode) {
        case TCode::WriteQuadletRequest:
        case TCode::ReadQuadletResponse:
        case TCode::CycleStart: {
            snprintf(buf, sizeof(buf), " = 0x%08x", header[3]);
            specific = buf;
            break;
        }
        case TCode::WriteBlockRequest:
        case TCode::ReadBlockRequest:
        case TCode::ReadBlockResponse:
        case TCode::LockRequest:
        case TCode::LockResponse: {
         snprintf(buf, sizeof(buf), " %u,0x%x", GetDataLength(header[3]), GetExtendedTCode(header[3]));
         specific = buf;
            break;
        }
        default:
            specific.clear();
        }
    }

    // Format packet details based on tcode
    snprintf(buf, sizeof(buf), "spd %u", speed);
    oss += buf;

    if (header.size() >= 2) {
        snprintf(buf, sizeof(buf), " tl %02x, 0x%04x → 0x%04x", GetTLabel(header[0]), GetSource(header[1]), GetDestination(header[0]));
        oss += buf;
    }

    oss += ", ";
    oss += std::string(tcodeName);

    // Add offset for requests
    if (header.size() >= 3 && (tcode == TCode::WriteQuadletRequest ||
                               tcode == TCode::WriteBlockRequest ||
                               tcode == TCode::ReadQuadletRequest ||
                               tcode == TCode::ReadBlockRequest ||
                               tcode == TCode::LockRequest)) {
        // Offset is 48-bit value; print as hex
        const uint64_t offset = GetOffset(header[1], header[2]);
        // Use two snprintf calls to format 48-bit offset
        char offbuf[32];
        snprintf(offbuf, sizeof(offbuf), ", offset 0x%llx", static_cast<long long unsigned>(offset));
        oss += offbuf;
    }

    oss += specific;

    return oss;
}

std::string DiagnosticLogger::DecodeEventCode(uint8_t eventCode) {
    // Adapted from Linux evts[] table (ohci.c lines 508-525)
    // Maps OHCI event codes to human-readable descriptions
    // Per OHCI §3.1.1: Event codes appear in ContextControl.event field
    static constexpr std::array<std::string_view, 33> kEventNames = {
        "evt_no_status",        // 0x00
        "-reserved-",           // 0x01
        "evt_long_packet",      // 0x02 - packet exceeds context buffer
        "evt_missing_ack",      // 0x03 - no acknowledge from target
        "evt_underrun",         // 0x04 - buffer underrun (transmit)
        "evt_overrun",          // 0x05 - buffer overrun (receive)
        "evt_descriptor_read",  // 0x06 - descriptor read error (CRITICAL)
        "evt_data_read",        // 0x07 - data read error (transmit)
        "evt_data_write",       // 0x08 - data write error (receive)
        "evt_bus_reset",        // 0x09 - bus reset detected
        "evt_timeout",          // 0x0A - transaction timeout
        "evt_tcode_err",        // 0x0B - invalid tcode
        "evt_reserved_0C",      // 0x0C
        "evt_reserved_0D",      // 0x0D
        "evt_unknown",          // 0x0E - unknown error
        "evt_flushed",          // 0x0F - packet flushed
        "evt_reserved_10",      // 0x10
        "ack_complete",         // 0x11 - ACK complete (success!)
        "ack_pending",          // 0x12 - ACK pending
        "evt_reserved_13",      // 0x13
        "ack_busy_X",           // 0x14 - ACK busy (retry X)
        "ack_busy_A",           // 0x15 - ACK busy (retry A)
        "ack_busy_B",           // 0x16 - ACK busy (retry B)
        "evt_reserved_17",      // 0x17
        "evt_reserved_18",      // 0x18
        "evt_reserved_19",      // 0x19
        "evt_reserved_1A",      // 0x1A
        "ack_tardy",            // 0x1B - ACK too late
        "evt_reserved_1C",      // 0x1C
        "ack_data_error",       // 0x1D - data CRC error
        "ack_type_error",       // 0x1E - invalid packet type
        "evt_reserved_1F",      // 0x1F
        "pending/cancelled",    // 0x20 - transaction cancelled
    };

    if (eventCode < kEventNames.size()) {
        return std::string(kEventNames[eventCode]);
    }
    
    char buf[32];
    snprintf(buf, sizeof(buf), "evt_unknown_0x%02x", static_cast<uint32_t>(eventCode));
    return std::string(buf);
}

std::string DiagnosticLogger::DecodePhyPacket(uint32_t phy0, uint32_t phy1) {
    // Decode PHY packet contents (IEEE 1394-2008 §16.3)
    // These appear in link-internal packets and PHY register responses
    std::string oss;
    char buf[128];
    
    const uint8_t phyId = (phy0 >> 24) & 0x3F;
    const uint8_t packetId = (phy0 >> 24) & 0xC0;
    
    snprintf(buf, sizeof(buf), "PHY packet: ID=%u", static_cast<uint32_t>(phyId));
    oss += buf;
    
    // Decode based on packet type
    if ((phy0 & 0xFF000000) == 0x00000000) {
        // Self-ID packet (handled separately by DecodeSelfIDSequence)
        oss += " (Self-ID)";
    } else if ((phy0 & 0xC0000000) == 0x40000000) {
        // PHY configuration packet (§16.3.3)
        oss += " PHY_CONFIG";
        const bool forceRoot = (phy0 & 0x00800000) != 0;
        const uint8_t rootId = (phy0 >> 24) & 0x3F;
        const uint8_t gapCount = (phy0 >> 16) & 0x3F;
        snprintf(buf, sizeof(buf), " root=%u%s gap=%u",
                 static_cast<uint32_t>(rootId),
                 forceRoot ? " FORCE" : "",
                 static_cast<uint32_t>(gapCount));
        oss += buf;
    } else if ((phy0 & 0xC0000000) == 0x80000000) {
        // Link-on packet (§16.3.4)
        oss += " LINK_ON";
    } else {
        snprintf(buf, sizeof(buf), " type=0x%02x", static_cast<uint32_t>(packetId));
        oss += buf;
    }
    
    snprintf(buf, sizeof(buf), " [0]=0x%08x [1]=0x%08x", phy0, phy1);
    oss += buf;
    
    return oss;
}

DiagnosticLogger::PortStatus DiagnosticLogger::GetPortStatus(std::span<const uint32_t> sequence,
                                                             size_t portIndex) {
    // Extract port status from Self-ID packet sequence
    // Per IEEE 1394a §4.3.4.1: ports 0-2 in quadlet 0, ports 3+ in extended quadlets

    if (portIndex < 3) {
        // Ports 0-2 are in bits [8-9], [10-11], [12-13] of first quadlet
        const uint32_t shift = 8 + static_cast<uint32_t>(portIndex) * 2;
        return static_cast<PortStatus>((sequence[0] >> shift) & 0x3);
    } else {
        // Ports 3-26 are in extended quadlets
        const size_t extPortIndex = portIndex - 3;
        const size_t quadletIndex = 1 + extPortIndex / 8;
        const size_t bitPairIndex = extPortIndex % 8;

        if (quadletIndex >= sequence.size()) {
            return PortStatus::None;
        }

        const uint32_t shift = 16 + static_cast<uint32_t>(bitPairIndex) * 2;
        return static_cast<PortStatus>((sequence[quadletIndex] >> shift) & 0x3);
    }
}

} // namespace ASFW::Driver
