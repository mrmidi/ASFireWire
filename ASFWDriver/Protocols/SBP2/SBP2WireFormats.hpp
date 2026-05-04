#pragma once

// SBP-2 (Serial Bus Protocol 2) wire-format definitions.
// Based on ANSI INCITS 335-1999 (SBP-2).
// All multi-byte fields are stored in **big-endian** (bus/wire) order.
// Use ToBusOrder / FromBusOrder from Core/PhyPackets.hpp or std::byteswap for conversion.

#include <array>
#include <cstdint>
#include <cstring>

namespace ASFW::Protocols::SBP2::Wire {

// ---------------------------------------------------------------------------
// Big-endian helpers (inline, constexpr)
// ---------------------------------------------------------------------------

[[nodiscard]] inline constexpr uint16_t ToBE16(uint16_t v) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(v);
#else
    return v;
#endif
}

[[nodiscard]] inline constexpr uint32_t ToBE32(uint32_t v) noexcept {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}

[[nodiscard]] inline constexpr uint16_t FromBE16(uint16_t v) noexcept { return ToBE16(v); }
[[nodiscard]] inline constexpr uint32_t FromBE32(uint32_t v) noexcept { return ToBE32(v); }

// ---------------------------------------------------------------------------
// SBP-2 Management Agent ORB types
// ---------------------------------------------------------------------------

// Login ORB — written to the management agent address to initiate login.
// Ref: SBP-2 §5.3.1
struct LoginORB {
    // Quadlet 0: password address (hi)
    uint32_t passwordAddressHi{0};
    // Quadlet 1: password address (lo)
    uint32_t passwordAddressLo{0};

    // Quadlet 2: login response address (hi)
    // [31:16] nodeID of response buffer, [15:0] addressHi
    uint32_t loginResponseAddressHi{0};
    // Quadlet 3: login response address (lo)
    uint32_t loginResponseAddressLo{0};

    // Quadlet 4: options + LUN
    // [31:16] options (notify/reconnect/exclusive), [15:0] LUN
    uint16_t options{0};
    uint16_t lun{0};

    // Quadlet 5: password length + login response length
    uint16_t passwordLength{0};
    uint16_t loginResponseLength{0};

    // Quadlet 6: status FIFO address (hi)
    uint32_t statusFIFOAddressHi{0};
    // Quadlet 7: status FIFO address (lo)
    uint32_t statusFIFOAddressLo{0};

    static constexpr uint32_t kSize = 32; // 8 quadlets
};

static_assert(sizeof(LoginORB) == LoginORB::kSize, "LoginORB must be 32 bytes");

// Login Response — device writes this after successful login.
// Ref: SBP-2 §5.3.2
struct LoginResponse {
    uint16_t length{0};       // [31:16] length
    uint16_t loginID{0};      // [15:0] login ID assigned by device
    uint32_t commandBlockAgentAddressHi{0};
    uint32_t commandBlockAgentAddressLo{0};
    uint16_t reserved{0};
    uint16_t reconnectHold{0}; // 2^reconnectHold seconds

    static constexpr uint32_t kSize = 16; // 4 quadlets
};

static_assert(sizeof(LoginResponse) == LoginResponse::kSize, "LoginResponse must be 16 bytes");

// Reconnect ORB — written to management agent to reconnect after bus reset.
// Ref: SBP-2 §5.3.4
struct ReconnectORB {
    uint32_t reserved1{0};
    uint32_t reserved2{0};
    uint32_t reserved3{0};
    uint32_t reserved4{0};

    // [31:16] options, [15:0] loginID
    uint16_t options{0};
    uint16_t loginID{0};

    uint32_t reserved5{0};

    uint32_t statusFIFOAddressHi{0};
    uint32_t statusFIFOAddressLo{0};

    static constexpr uint32_t kSize = 32; // 8 quadlets
};

static_assert(sizeof(ReconnectORB) == ReconnectORB::kSize, "ReconnectORB must be 32 bytes");

// Logout ORB — written to management agent to terminate login session.
// Ref: SBP-2 §5.3.5
struct LogoutORB {
    uint32_t reserved1{0};
    uint32_t reserved2{0};
    uint32_t reserved3{0};
    uint32_t reserved4{0};

    // [31:16] options, [15:0] loginID
    uint16_t options{0};
    uint16_t loginID{0};

    uint32_t reserved5{0};

    uint32_t statusFIFOAddressHi{0};
    uint32_t statusFIFOAddressLo{0};

    static constexpr uint32_t kSize = 32; // 8 quadlets
};

static_assert(sizeof(LogoutORB) == LogoutORB::kSize, "LogoutORB must be 32 bytes");

// ---------------------------------------------------------------------------
// SBP-2 Normal Command ORB
// Ref: SBP-2 §5.1.1
// ---------------------------------------------------------------------------

struct NormalORB {
    // Quadlet 0-1: next ORB pointer
    uint32_t nextORBAddressHi{0};  // [31] = 1 => null (no next ORB)
    uint32_t nextORBAddressLo{0};

    // Quadlet 2-3: data descriptor (page table or direct buffer)
    uint32_t dataDescriptorHi{0};
    uint32_t dataDescriptorLo{0};

    // Quadlet 4: options + data size
    // [15] notify, [13:12] rq_fmt, [11] direction, [9:8] speed,
    // [7:4] max payload size (log2 in quadlets), [3:2] page table format, [1:0] reserved
    uint16_t options{0};
    uint16_t dataSize{0};

    // Command block follows (variable length, up to maxCommandBlockSize)
    // Access via CommandBlock() helper.

    [[nodiscard]] uint32_t* CommandBlock() noexcept {
        return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + 16);
    }
    [[nodiscard]] const uint32_t* CommandBlock() const noexcept {
        return reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(this) + 16);
    }

    // Minimum ORB size (no command block)
    static constexpr uint32_t kHeaderSize = 16;
    // Null next-ORB indicator (bit 31 set in hi address)
    static constexpr uint32_t kNextORBNull = 0x80000000u;
};

// Page Table Entry — maps data buffer for DMA.
// Ref: SBP-2 §5.1.2
struct PageTableEntry {
    uint16_t segmentLength{0};
    uint16_t segmentBaseAddressHi{0};
    uint32_t segmentBaseAddressLo{0};

    static constexpr uint32_t kSize = 8;
};

static_assert(sizeof(PageTableEntry) == PageTableEntry::kSize, "PTE must be 8 bytes");

// ---------------------------------------------------------------------------
// SBP-2 Status Block
// Ref: SBP-2 §5.2
// ---------------------------------------------------------------------------

struct StatusBlock {
    uint8_t  details{0};     // [7] Src, [6:4] Resp, [3:2] D, [1:0] Len
    uint8_t  sbpStatus{0};   // SBP-2 specific status code
    uint16_t orbOffsetHi{0};
    uint32_t orbOffsetLo{0};
    uint32_t status[6]{};    // Up to 24 additional bytes of status

    static constexpr uint32_t kMaxSize = 32; // header (8) + max status (24)

    [[nodiscard]] uint8_t Source() const noexcept { return (details >> 7) & 0x1; }
    [[nodiscard]] uint8_t Response() const noexcept { return (details >> 4) & 0x7; }
    [[nodiscard]] uint8_t DeadBit() const noexcept { return (details >> 2) & 0x1; }
    [[nodiscard]] uint8_t Length() const noexcept { return details & 0x3; }
};

static_assert(sizeof(StatusBlock) == 32, "StatusBlock must be 32 bytes");

// ---------------------------------------------------------------------------
// SBP-2 Management ORB (Task Management)
// Ref: SBP-2 §6.2
// ---------------------------------------------------------------------------

struct TaskManagementORB {
    uint32_t orbOffsetHi{0};
    uint32_t orbOffsetLo{0};
    uint32_t reserved1[2]{};
    uint16_t options{0};
    uint16_t loginID{0};
    uint32_t reserved2{0};
    uint32_t statusFIFOAddressHi{0};
    uint32_t statusFIFOAddressLo{0};

    static constexpr uint32_t kSize = 32;
};

static_assert(sizeof(TaskManagementORB) == TaskManagementORB::kSize);

// ---------------------------------------------------------------------------
// Management Agent address calculation
// ---------------------------------------------------------------------------

// Management Agent registers start at 0xF0000000 + (managementOffset << 2).
// The management offset comes from the Config ROM Unit_Directory entry.
[[nodiscard]] inline constexpr uint32_t ManagementAgentAddressLo(uint32_t managementOffset) noexcept {
    return 0xF0000000u + (managementOffset << 2);
}

// SBP-2 ORBs embed a full 16-bit node ID in bus addresses. ASFW's generic
// bus-info path may expose only the local 6-bit physical node number, so expand
// it to the local-bus form (0xffc0 | phyId) before writing ORBs.
[[nodiscard]] inline constexpr uint16_t NormalizeBusNodeID(uint16_t nodeID) noexcept {
    if ((nodeID & 0xFFC0u) == 0xFFC0u) {
        return nodeID;
    }
    return static_cast<uint16_t>(0xFFC0u | (nodeID & 0x003Fu));
}

[[nodiscard]] inline constexpr uint32_t ComposeBusAddressHi(uint16_t nodeID,
                                                            uint16_t addressHi) noexcept {
    return (static_cast<uint32_t>(NormalizeBusNodeID(nodeID)) << 16) |
           static_cast<uint32_t>(addressHi);
}

// Command Block Agent register offsets (relative to agent base from login response).
struct CommandBlockAgentOffsets {
    static constexpr uint32_t kAgentReset               = 0x04; // Fetch agent reset (quadlet write)
    static constexpr uint32_t kFetchAgent               = 0x08; // ORB pointer write (fetch agent, non-fast-start)
    static constexpr uint32_t kDoorbell                  = 0x10; // Doorbell ring (quadlet write)
    static constexpr uint32_t kUnsolicitedStatusEnable   = 0x14; // Re-enable unsolicited status
};

// ---------------------------------------------------------------------------
// SBP-2 ORB options bit helpers (big-endian accessors)
// ---------------------------------------------------------------------------

namespace Options {
    // Login ORB options
    static constexpr uint16_t kLoginNotify    = ToBE16(0x8000);
    static constexpr uint16_t kExclusiveLogin = ToBE16(0x1000);

    // Reconnect ORB options
    static constexpr uint16_t kReconnectNotify = ToBE16(0x8003); // reconnect + notify

    // Logout ORB options
    static constexpr uint16_t kLogoutNotify = ToBE16(0x8007); // logout + notify

    // Normal ORB options
    static constexpr uint16_t kNotify         = ToBE16(0x8000);
    static constexpr uint16_t kDirectionRead  = ToBE16(0x0800); // data from target
    static constexpr uint16_t kSpeedShift     = 8;
    static constexpr uint16_t kSpeed100       = ToBE16(0x0000);
    static constexpr uint16_t kSpeed200       = ToBE16(0x0100);
    static constexpr uint16_t kSpeed400       = ToBE16(0x0200);
    static constexpr uint16_t kSpeed800       = ToBE16(0x0300);
    static constexpr uint16_t kMaxPayloadShift = 4;
    static constexpr uint16_t kPageTableUnrestricted = ToBE16(0x0008);

    // Management ORB function codes
    static constexpr uint32_t kFunctionQueryLogins    = 1;
    static constexpr uint32_t kFunctionAbortTask      = 0xB;
    static constexpr uint32_t kFunctionAbortTaskSet   = 0xC;
    static constexpr uint32_t kFunctionLogicalUnitReset = 0xE;
    static constexpr uint32_t kFunctionTargetReset    = 0xF;
}

// SBP-2 status codes (from sbpStatus field)
namespace SBPStatus {
    static constexpr uint8_t kNoAdditionalInfo   = 0;
    static constexpr uint8_t kReqTypeNotSupported = 1;
    static constexpr uint8_t kSpeedNotSupported   = 2;
    static constexpr uint8_t kPageSizeNotSupported = 3;
    static constexpr uint8_t kAccessDenied        = 4;
    static constexpr uint8_t kResourceUnavailable = 5;
    static constexpr uint8_t kFunctionRejected    = 6;
    static constexpr uint8_t kLoginIDNotRecognized = 7;
    static constexpr uint8_t kDummyORBCompleted   = 8;
    static constexpr uint8_t kRequestAborted      = 0xB;
    static constexpr uint8_t kUnspecifiedError    = 0xFF;
}

// Busy timeout register (CSR address 0xFFFFF0000210)
static constexpr uint32_t kBusyTimeoutAddressHi = 0x0000FFFFu;
static constexpr uint32_t kBusyTimeoutAddressLo = 0xF0000210u;

} // namespace ASFW::Protocols::SBP2::Wire
