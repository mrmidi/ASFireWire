#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "TopologyTypes.hpp"  // For PortState enum

namespace ASFW::Driver {

// Snapshot of OHCI interrupt registers captured in the ISR before routing
// onto the single-threaded controller queue described in DRAFT.md §5.3.
struct InterruptSnapshot {
    uint32_t intEvent{0};
    uint32_t intMask{0};
    uint32_t isoXmitEvent{0};
    uint32_t isoRecvEvent{0};
    uint64_t timestamp{0};
};

// Aggregated bus reset metrics surfaced via the DriverKit status methods.
struct BusResetMetrics {
    uint64_t lastResetStart{0};
    uint64_t lastResetCompletion{0};
    uint32_t resetCount{0};
    uint32_t abortCount{0};
    std::optional<std::string> lastFailureReason;
};

// Self-ID capture metrics for diagnostics and GUI export
struct SelfIDMetrics {
    std::vector<uint32_t> rawQuadlets;           // Raw Self-ID buffer capture
    std::vector<std::pair<size_t, unsigned int>> sequences;  // Sequence indices (start, count)
    uint32_t generation{0};
    uint64_t captureTimestamp{0};
    bool valid{false};
    bool timedOut{false};
    bool crcError{false};
    std::optional<std::string> errorReason;
};

// Node descriptor with port states for topology visualization
struct TopologyNode {
    uint8_t nodeId{0};
    uint8_t portCount{0};
    uint32_t maxSpeedMbps{0};
    bool isIRMCandidate{false};
    bool linkActive{false};
    bool initiatedReset{false};
    bool isRoot{false};
    uint8_t gapCount{0};
    uint8_t powerClass{0};  // PowerClass enum value
    std::vector<PortState> portStates;  // Port state for each port (p0..p15)
    std::optional<uint8_t> parentPort;  // Port connected to parent (for tree)

    // Tree structure links (NEW - ported from ASFireWire/ASOHCI/Core/Topology.cpp)
    std::vector<uint8_t> parentNodeIds;  // Usually 0 or 1 parent (root has 0)
    std::vector<uint8_t> childNodeIds;   // Connected child nodes
};

// Immutable topology snapshot exchanged between SelfID decode and
// higher-level consumers (UI, diagnostics, tests).
struct TopologySnapshot {
    uint32_t generation{0};
    std::vector<TopologyNode> nodes;
    uint64_t capturedAt{0};

    // Topology analysis results per IEEE 1394-1995 §8.4
    std::optional<uint8_t> rootNodeId;      // Highest nodeID with active link
    std::optional<uint8_t> irmNodeId;       // IRM-capable node with highest nodeID
    std::optional<uint8_t> localNodeId;     // Our node ID (if valid)
    uint8_t gapCount{63};                   // Optimum gap count for this topology
    uint8_t nodeCount{0};                   // Total nodes with valid Self-ID
    uint8_t maxHopsFromRoot{0};             // NEW: Maximum hop count from root node

    // Bus info derived from OHCI NodeID register
    // busBase16 = (bus << 6), ready to OR with a 6-bit node to form a 16-bit Node_ID
    uint16_t busBase16{0};
    // Optional decoded bus number (0..1023). std::nullopt if NodeID invalid.
    std::optional<uint16_t> busNumber;

    // Self-ID raw data for GUI export
    SelfIDMetrics selfIDData;               // Complete Self-ID capture
    std::vector<std::string> warnings;      // Topology validation warnings
};

// Helper: Compose a full 16-bit Node_ID from bus base and 6-bit node number
static inline uint16_t ComposeNodeID(uint16_t busBase16, uint8_t node6) {
    return static_cast<uint16_t>((busBase16 & 0xFFC0u) | (node6 & 0x3Fu));
}

// Unified status payload returned by CopyStatus-style IIG commands.
struct ControllerStatusSummary {
    std::string stateName;
    BusResetMetrics busMetrics;
    std::optional<TopologySnapshot> topology;
};

// ---------------------------------------------------------------------------
// Shared status block exported via shared memory for GUI consumption.
// ---------------------------------------------------------------------------

enum class SharedStatusReason : uint32_t {
    Boot            = 1,
    Interrupt       = 2,
    BusReset        = 3,
    AsyncActivity   = 4,
    Watchdog        = 5,
    Manual          = 6,
    Disconnect      = 7,
};

struct SharedStatusBlock {
    static constexpr uint32_t kVersion = 1;

    uint32_t version{SharedStatusBlock::kVersion};
    uint32_t length{sizeof(SharedStatusBlock)};
    uint64_t sequence{0};
    uint64_t updateTimestamp{0};    // mach_absolute_time()
    uint32_t reason{static_cast<uint32_t>(SharedStatusReason::Boot)};
    uint32_t detailMask{0};         // Raw interrupt mask or other context

    char controllerStateName[32]{}; // Null-terminated state string
    uint32_t controllerState{0};    // ControllerState enum value
    uint32_t flags{0};              // Bitfield (see FlagBits)

    uint32_t busGeneration{0};
    uint32_t nodeCount{0};
    uint32_t localNodeID{0xFFFFFFFFu};
    uint32_t rootNodeID{0xFFFFFFFFu};
    uint32_t irmNodeID{0xFFFFFFFFu};

    uint64_t busResetCount{0};
    uint64_t lastBusResetStart{0};
    uint64_t lastBusResetCompletion{0};

    uint64_t asyncLastCompletion{0};  // mach time of last completion observed
    uint32_t asyncPending{0};         // Outstanding slots still active
    uint32_t asyncTimeouts{0};        // Total timeouts observed

    uint64_t watchdogTickCount{0};
    uint64_t watchdogLastTickUsec{0};

    uint8_t reserved[104]{};          // Pad to 256 bytes for future expansion

    enum FlagBits : uint32_t {
        kFlagIsIRM         = 1u << 0,
        kFlagIsCycleMaster = 1u << 1,
        kFlagLinkActive    = 1u << 2,
    };
};
static_assert(sizeof(SharedStatusBlock) == 256, "SharedStatusBlock must remain 256 bytes");

} // namespace ASFW::Driver
