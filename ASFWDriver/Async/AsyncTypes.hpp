#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <functional>
#include <span>
#include <DriverKit/IOReturn.h>

// Forward declaration - we'll define FWAddress helpers before FWAddress uses them
namespace ASFW::Async {
    struct FWAddress;
}

// Define FW namespace helpers that depend on FWAddress (forward declared)
// These are defined here to avoid circular dependency with FWCommon.hpp
namespace ASFW::FW {

/**
 * Pack FWAddress into 64-bit target address.
 * Format: bits[63:48] = nodeID, bits[47:32] = addressHi, bits[31:0] = addressLo
 */
[[nodiscard]] constexpr uint64_t Pack(const ::ASFW::Async::FWAddress& addr);

/**
 * Unpack 64-bit target address into FWAddress.
 * Format: bits[63:48] = nodeID, bits[47:32] = addressHi, bits[31:0] = addressLo
 */
[[nodiscard]] constexpr ::ASFW::Async::FWAddress Unpack(uint64_t target);

/**
 * Convert FWAddress to 64-bit target address (alias for Pack).
 */
[[nodiscard]] constexpr uint64_t ToU64(const ::ASFW::Async::FWAddress& addr);

/**
 * Format FWAddress as string for logging (e.g., "0xffff:f0000400 (node=0x0001)").
 */
inline std::string AddressToString(const ::ASFW::Async::FWAddress& addr);

} // namespace ASFW::FW

namespace ASFW::Async {

/**
 * An opaque handle representing an in-flight asynchronous transaction.
 * Use a 32-bit FWHandle wrapper to provide type-safety and preserve
 * the previous `.value` member used across the codebase.
 */
struct FWHandle {
    uint32_t value{0};
    explicit operator bool() const { return value != 0; }
};
using AsyncHandle = FWHandle;

// Diagnostics snapshot structures -------------------------------------------------

struct AsyncDescriptorStatus {
    uint64_t descriptorVirt{0};   ///< CPU-accessible base address of descriptor ring
    uint64_t descriptorIOVA{0};   ///< Device-visible base address written to CommandPtr
    uint32_t descriptorCount{0};  ///< Number of descriptors in the ring (including sentinel if present)
    uint32_t descriptorStride{0}; ///< Size in bytes of each descriptor element
    uint32_t commandPtr{0};       ///< Last CommandPtr value written to hardware (low 32 bits)
    uint32_t reserved{0};
};

struct AsyncBufferStatus {
    uint64_t bufferVirt{0};   ///< CPU-accessible base of data buffer pool (0 if not applicable)
    uint64_t bufferIOVA{0};   ///< Device-visible base of data buffer pool (0 if not applicable)
    uint32_t bufferCount{0};  ///< Number of buffers in pool (0 if not applicable)
    uint32_t bufferSize{0};   ///< Size in bytes for each buffer (0 if not applicable)
};

struct AsyncStatusSnapshot {
    AsyncDescriptorStatus atRequest{};
    AsyncDescriptorStatus atResponse{};
    AsyncDescriptorStatus arRequest{};
    AsyncDescriptorStatus arResponse{};
    AsyncBufferStatus arRequestBuffers{};
    AsyncBufferStatus arResponseBuffers{};
    uint64_t dmaSlabVirt{0};
    uint64_t dmaSlabIOVA{0};
    uint32_t dmaSlabSize{0};
    uint32_t reserved{0};
};

// Backwards-compatible handle packing helpers (operate on the new 32-bit FWHandle alias).
// These preserve the previous public helper names so existing call-sites continue to compile.
namespace detail {
    constexpr uint32_t kIndexMask = 0x0FFFu; // 12 bits
    constexpr uint32_t kGenMask = 0xF000u;   // high 4 bits of low 16-bit field
    constexpr unsigned kGenShift = 12u;

    [[nodiscard]] inline uint32_t MakeHandle(uint16_t index12, uint16_t gen4) {
        uint32_t v = static_cast<uint32_t>(((static_cast<uint32_t>(gen4) & 0xFu) << kGenShift) | (static_cast<uint32_t>(index12) & kIndexMask));
        // Reserve 0 as invalid; if the combination yields 0, set gen=1
        if (v == 0) v = static_cast<uint32_t>(((1u << kGenShift) & kGenMask) | (static_cast<uint32_t>(index12) & kIndexMask));
        return v;
    }

    [[nodiscard]] inline uint16_t HandleIndex(AsyncHandle h) {
        return static_cast<uint16_t>(static_cast<uint32_t>(h.value) & kIndexMask);
    }

    [[nodiscard]] inline uint16_t HandleGen(AsyncHandle h) {
        return static_cast<uint16_t>((static_cast<uint32_t>(h.value) & kGenMask) >> kGenShift);
    }
} // namespace detail

/**
 * User-facing outcome for asynchronous transactions. Maps hardware ack/event codes and
 * internal driver states into a compact status enum.
 */
enum class AsyncStatus : uint8_t {
    kSuccess = 0,
    kTimeout,
    kShortRead,
    kBusyRetryExhausted,
    kAborted,
    kHardwareError,
    kLockCompareFail,
    kStaleGeneration,
};

/**
 * FWAddress - Standard FireWire 48-bit address structure
 * Ported from IOFireWireFamily/IOFireWireFamilyCommon.h for API compatibility.
 *
 * Format: nodeID[15:0] + addressHi[15:0] + addressLo[31:0] = 64 bits total
 *   - nodeID: bus[15:10] | node[5:0]
 *   - addressHi: upper 16 bits of 48-bit IEEE 1394 address space
 *   - addressLo: lower 32 bits of 48-bit IEEE 1394 address space
 *
 * Default constructor creates invalid address (0xdead:0xcafebabe) per Apple convention.
 */
struct FWAddress {
    uint16_t nodeID{0};      ///< Bus/node identifier (bus[15:10], node[5:0])
    uint16_t addressHi{0};   ///< Top 16 bits of 48-bit address
    uint32_t addressLo{0};   ///< Bottom 32 bits of 48-bit address

    /// Default constructor: invalid address (0xdead:0xcafebabe per Apple)
    FWAddress() : nodeID(0), addressHi(0xdead), addressLo(0xcafebabe) {}

    /// Constructor with address only (nodeID defaults to 0)
    FWAddress(uint16_t h, uint32_t l) : nodeID(0), addressHi(h), addressLo(l) {}

    /// Full constructor with nodeID
    FWAddress(uint16_t h, uint32_t l, uint16_t n) : nodeID(n), addressHi(h), addressLo(l) {}

    /// Copy constructor
    FWAddress(const FWAddress& a) : nodeID(a.nodeID), addressHi(a.addressHi), addressLo(a.addressLo) {}

    /// Create FWAddress from 64-bit target (Apple IOFWUserCommand pattern)
    /// @param target 64-bit address: bits[63:48] = nodeID, bits[47:32] = addressHi, bits[31:0] = addressLo
    /// @param nodeIDOverride Optional override for nodeID (if provided, overrides target[63:48])
    static FWAddress FromU64(uint64_t target, uint16_t nodeIDOverride = 0) {
        FWAddress addr = FW::Unpack(target);
        if (nodeIDOverride != 0) {
            addr.nodeID = nodeIDOverride;
        }
        return addr;
    }

    /// Convert FWAddress to 64-bit target address (uses FW::Pack helper).
    uint64_t ToU64() const {
        return FW::Pack(*this);
    }
};

} // namespace ASFW::Async

// Implement FW namespace helpers (definitions)
namespace ASFW::FW {

[[nodiscard]] constexpr uint64_t Pack(const ::ASFW::Async::FWAddress& addr) {
    return (uint64_t(addr.nodeID) << 48) |
           (uint64_t(addr.addressHi) << 32) |
           uint64_t(addr.addressLo);
}

[[nodiscard]] constexpr ::ASFW::Async::FWAddress Unpack(uint64_t target) {
    ::ASFW::Async::FWAddress addr;
    addr.nodeID = static_cast<uint16_t>((target >> 48) & 0xFFFFu);
    addr.addressHi = static_cast<uint16_t>((target >> 32) & 0xFFFFu);
    addr.addressLo = static_cast<uint32_t>(target & 0xFFFFFFFFu);
    return addr;
}

[[nodiscard]] constexpr uint64_t ToU64(const ::ASFW::Async::FWAddress& addr) {
    return Pack(addr);
}

inline std::string AddressToString(const ::ASFW::Async::FWAddress& addr) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "0x%04x:%08x (node=0x%04x)",
                  addr.addressHi, addr.addressLo, addr.nodeID);
    return std::string(buf);
}

} // namespace ASFW::FW

// Now include FWCommon.hpp for other constants and helpers
#include "../Core/FWCommon.hpp"

namespace ASFW::Async {

/**
 * AsyncCmdOptions - Apple-mirrored command options contract
 * Matches IOFWUserCommand.h lines 79-106 and IOFWUserCommand.cpp submit() patterns.
 * 
 * Reference: IOFWUserCommand.cpp submit() uses flags parameter:
 *   - kFWCommandInterfaceSyncExecute (syncExecute)
 *   - kFireWireCommandUseCopy (useCopy)
 *   - kFireWireCommandAbsolute (absolute)
 *   - setFlush() (needsFlush) - line 412-422
 *   - kFWCommandInterfaceForceBlockRequest (forceBlock)
 */
struct AsyncCmdOptions {
    bool syncExecute{false};      ///< kFWCommandInterfaceSyncExecute - block until complete
    bool useCopy{false};          ///< kFireWireCommandUseCopy - inline payload (quadlets)
    bool absolute{false};         ///< kFireWireCommandAbsolute - includes generation
    bool failOnReset{false};      ///< IOFWUserCommand.cpp line 122 - fail if bus resets
    bool needsFlush{true};        ///< setFlush() - true=immediate stop, false=keep running
    bool forceBlock{false};       ///< kFWCommandInterfaceForceBlockRequest - force block transfer
    uint32_t timeoutMs{1000};     ///< setTimeout() - transaction timeout in milliseconds
    uint8_t retries{0};           ///< setRetries() - max retry attempts
    uint8_t maxSpeed{0};          ///< setMaxSpeed() - speed code (0=S100, 1=S200, 2=S400, 3=S800)
    uint16_t maxPacket{0};        ///< setMaxPacket() - max packet size (0=auto)
};

/**
 * AsyncCmdResult - Apple-mirrored command result contract
 * Matches IOFWUserCommand.cpp CommandSubmitResult and async completion patterns.
 * 
 * Reference: IOFWUserCommand.cpp asyncReadWriteCommandCompletion() lines 97-109
 *            IOFWUserCommand.cpp asyncReadQuadletCommandCompletion() lines 121-130
 */
struct AsyncCmdResult {
    IOReturn status{kIOReturnSuccess};   ///< IOKit return code
    uint32_t bytesTransferred{0};        ///< Actual bytes transferred
    uint8_t ackCode{0};                  ///< IEEE 1394 ack code (ACK_COMPLETE, etc.)
    uint8_t responseCode{0};             ///< IEEE 1394 response code (rCode)
    bool locked{false};                  ///< For compare-swap: true if lock succeeded
    uint32_t lockValueLo{0};             ///< For compare-swap: low 32 bits of read value
    uint32_t lockValueHi{0};             ///< For compare-swap: high 32 bits of read value
};

/**
 * Retry policy configuration for async transactions.
 * Matches Apple's IOFWAsyncCommand retry pattern (fCurRetries/fMaxRetries) but modernized
 * for DriverKit with explicit policy objects instead of per-command counters.
 *
 * Reference: IOFWAsyncCommand.cpp lines 285-305 (retry logic in complete())
 *            IOFWCommand.h lines 314-315 (fCurRetries/fMaxRetries fields)
 */
struct RetryPolicy {
    uint8_t maxRetries{3};           ///< Max retry attempts (Apple default: kFWCmdDefaultRetries = 3)
    uint64_t retryDelayUsec{1000};   ///< Delay between retries in microseconds (1ms default)
    bool retryOnBusy{true};          ///< Retry on ACK_BUSY_X/A/B (Apple default: yes)
    bool retryOnTimeout{true};       ///< Retry on timeout (Apple default: yes)
    bool speedFallback{false};       ///< Downgrade speed on type error like Apple (ROM quirks)
    
    /// Default policy: 3 retries, 1ms delay (Apple's kFWCmdDefaultRetries)
    static RetryPolicy Default() { return {3, 1000, true, true, false}; }
    
    /// Reduced retries for unreliable operations (Apple's kFWCmdReducedRetries = 2)
    static RetryPolicy Reduced() { return {2, 500, true, false, false}; }
    
    /// No retries (Apple's kFWCmdZeroRetries = 0)
    static RetryPolicy None() { return {0, 0, false, false, false}; }
    
    /// Increased retries for challenging operations (Apple's kFWCmdIncreasedRetries = 6)
    static RetryPolicy Increased() { return {6, 1000, true, true, true}; }
};

/**
 * PacketContext - IEEE 1394 packet construction parameters.
 * Contains source node ID, generation, and speed code for building packet headers.
 * Used by PacketBuilder to construct OHCI-compliant packet headers.
 */
struct PacketContext {
    uint16_t sourceNodeID{0};   ///< Local node ID (bus[15:10] | node[5:0])
    uint8_t generation{0};      ///< 8-bit bus generation counter
    uint8_t speedCode{0};       ///< Speed: 0=S100, 1=S200, 2=S400, 3=S800
};

/**
 * TransactionContext - Pre-validated bus state snapshot for transaction submission.
 * 
 * Obtained via AsyncSubsystem::PrepareTransactionContext() matching Apple's
 * executeCommandElement() gate check pattern (see DECOMPILATION.md §Command Execution).
 * Ensures:
 *   - NodeID valid bit set (OHCI NodeID register bit 31)
 *   - Bus not in reset (atomic flag check)
 *   - Generation stable (8-bit counter from GenerationTracker)
 *   - Speed code resolved (topology query or default S100)
 * 
 * Reference: AppleFWOHCI::executeCommandElement @ 0xDBBE validates controller
 *            awake and service running before accessing pending list.
 */
struct TransactionContext {
    uint16_t sourceNodeID{0};      ///< Local node ID with valid bit confirmed
    uint8_t generation{0};         ///< Current bus generation (8-bit)
    uint8_t speedCode{0};          ///< Transaction speed (0=S100...3=S800)
    PacketContext packetContext{}; ///< Packet builder parameters (convenience)
};

/** Parameters for an asynchronous read request (quadlet or block). */
struct ReadParams {
    uint16_t destinationID{0};
    uint32_t addressHigh{0};
    uint32_t addressLow{0};
    uint32_t length{0};
    uint8_t speedCode{0xFF};  // 0xFF = use context default, else 0=S100, 1=S200, 2=S400, 3=S800
};

/** Parameters for an asynchronous write request (quadlet or block). */
struct WriteParams {
    uint16_t destinationID{0};
    uint32_t addressHigh{0};
    uint32_t addressLow{0};
    const void* payload{nullptr};
    uint32_t length{0};
    uint8_t speedCode{0xFF};  // 0xFF = use context default, else 0=S100, 1=S200, 2=S400, 3=S800
};

/** Parameters for an asynchronous lock (compare-and-swap) request. */
struct LockParams {
    uint16_t destinationID{0};
    uint32_t addressHigh{0};
    uint32_t addressLow{0};
    const void* operand{nullptr};
    uint32_t length{0};
    uint8_t speedCode{0xFF};  // 0xFF = use context default, else 0=S100, 1=S200, 2=S400, 3=S800
};

/** Parameters for a fire-and-forget asynchronous stream packet. */
struct StreamParams {
    uint32_t channel{0};
    const void* payload{nullptr};
    uint32_t length{0};
};

/** Parameters for a PHY configuration packet. */
struct PhyParams {
    uint32_t quadlet1{0};
    uint32_t quadlet2{0};
};

/**
 * Completion callback invoked when an async transaction reaches a terminal state.
 * responsePayload is populated for reads and locks when data returns.
 *
 * Phase 2.3: Modernized with std::function for type-safe callbacks.
 * - Removed void* context (captured in lambda)
 * - Changed const void* + uint32_t to std::span<const uint8_t>
 *
 * Usage:
 *   auto cb = [this](AsyncHandle h, AsyncStatus s, std::span<const uint8_t> data) {
 *       if (s == AsyncStatus::Success) {
 *           ProcessResponse(data);
 *       }
 *   };
 *   subsystem.Read(params, cb);
 */
using CompletionCallback = std::function<void(AsyncHandle handle,
                                              AsyncStatus status,
                                              std::span<const uint8_t> responsePayload)>;

} // namespace ASFW::Async

/*
 * OHCI Specification References (IEEE 1394 Open HCI 1.1):
 * - Async transactions, headers, and payload rules: Chapter 7 (Transmit) and Chapter 8 (Receive).
 * - Status/event codes surfaced via ContextControl.event_code: Section 3.1.1, Table 3-2.
 * - Read request formats: Section 7.8.1.1 (Figures 7-9, 7-11).
 * - Write request formats: Section 7.8.1.2 (Figures 7-10, 7-12).
 * - Lock request formats: Section 7.8.1.3 (Figure 7-13).
 * - PHY packet transmit: Section 7.8.1.4 (Figure 7-14).
 * - Async stream packets: Section 7.8.3 (Figure 7-19).
 */
