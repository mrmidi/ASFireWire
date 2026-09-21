#pragma once

#include "../Common/FWCommon.hpp"
#include "DiscoveryValues.hpp"  // FwSpeed enum and constants
#include "RuntimeIdentity.hpp"  // DeviceInstanceId / UnitInstanceId
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ASFW::Discovery {

// ============================================================================
// Addressing & Identity
// ============================================================================

using Generation = ASFW::FW::Generation;
using Guid64 = uint64_t;
inline constexpr uint16_t kInvalidNodeId = 0xFFFFu;

[[nodiscard]] inline constexpr std::optional<uint8_t>
TryOperationalNodeId(uint16_t nodeId) noexcept {
    if (nodeId > 0xFFu) {
        return std::nullopt;
    }
    return static_cast<uint8_t>(nodeId);
}

struct FwAddress {
    struct BusNodeParts {
        uint16_t bus{0};
        uint8_t node{0xFF};
    };

    uint16_t bus{0};
    uint16_t node{0xFFFF};
    
    FwAddress() = default;
    constexpr explicit FwAddress(BusNodeParts parts) noexcept : bus(parts.bus), node(parts.node) {}
};

// ============================================================================
// Speed & Link Policy
// ============================================================================
// FwSpeed enum is now defined in DiscoveryValues.hpp

struct LinkPolicy {
    // Async speed. Starts at the topology speed and is demoted by SpeedPolicy
    // when a request times out, which is the right behaviour for asynchronous
    // requests — some devices genuinely mishandle them above S200 — and is
    // Apple's `fSpeedVector`, read by async transmit at
    // IOFireWireController.cpp:7058 and demoted at :2755-2759.
    //
    // TODO: S100 hardcoded for maximum hardware compatibility.
    FwSpeed localToNode{FwSpeed::S100};

    // Isochronous speed: the Self-ID path speed to this node, never demoted by
    // async outcomes. Apple resolves isoch speed from the PHY rather than the
    // speed vector (IOFWIsochChannel.cpp:653), because a device that refuses
    // async requests at S400 has said nothing about its isochronous receiver.
    // Conflating the two halves the isochronous bandwidth budget for free:
    // the charge is `unitsAtS1600 >> speedCode`, so S200 costs twice S400.
    FwSpeed isochToNode{FwSpeed::S100};

    uint16_t maxPayloadBytes{512};           // Clamp for Async TX (depends on MaxRec, speed, policy)
    bool halvePackets{false};                // Stability escape hatch
};

// ============================================================================
// Config ROM Structure (IEEE 1394-1995 §8.3, OHCI §7.8)
// ============================================================================

enum class ConfigROMFormat : uint8_t {
    Unknown,
    Minimal1212,
    General1394,
};

// Bus Info Block (BIB) - IEEE 1394 general Config ROMs use q0..q4 at minimum.
// Located at address 0xFFFFF0000400. True IEEE 1212 minimal ROMs are q0-only
// and do not carry the IEEE 1394 GUID/options fields.
struct BusInfoBlock {
    ConfigROMFormat format{ConfigROMFormat::Unknown};

    // BIB header quadlet (quadlet 0) - IEEE 1212
    uint8_t busInfoLength{0};    // [31:24] quadlets following header in BIB
    uint8_t crcLength{0};        // [23:16] quadlets covered by CRC (starting at quadlet 1)
    uint16_t crc{0};             // [15:0] CRC-16 value

    // BIB bus options quadlet (quadlet 2) - TA 1999027
    bool irmc{false};
    bool cmc{false};
    bool isc{false};
    bool bmc{false};
    bool pmc{false};

    uint8_t cycClkAcc{0};        // [23:16]
    uint8_t maxRec{0};           // [15:12]
    uint8_t maxRom{0};           // [9:8]
    uint8_t generation{0};       // [7:4]
    uint8_t linkSpd{0};          // [2:0]

    uint64_t guid{0};            // BIB[3:4] - Global unique identifier (64-bit)
};

// Config ROM directory entry keys (IEEE 1394-1995 §8.3.2)
// Minimal set for audio device classification
enum class CfgKey : uint8_t {
    TextDescriptor = 0x01,
    VendorId = 0x03,
    ModelId = 0x17,
    Unit_Spec_Id = 0x12,
    Unit_Sw_Version = 0x13,
    Logical_Unit_Number = 0x14,
    Node_Capabilities = 0x0C,
    Unit_Directory = 0xD1,  // IEEE 1212 Unit_Directory (keyId=0x11 when keyType=3)
    Management_Agent_Offset = 0x54,  // SBP-2 (keyType=CSR offset, keyId=0x14)
    Unit_Characteristics   = 0x39,  // SBP-2 (immediate in unit directory)
    Fast_Start             = 0x3A,  // SBP-2 (leaf in unit directory)
};

struct RomEntry {
    CfgKey key;
    uint32_t value;
    uint8_t entryType{0};  // 0=immediate, 1=CSR offset, 2=leaf, 3=directory
    uint32_t leafOffsetQuadlets{0};  // Target offset (quadlets) relative to directory header (for leaf/dir entries)
};

struct UnitDirectory {
    // Offset in quadlets relative to the start of the root directory (header quadlet).
    uint32_t offsetQuadlets{0};

    // IEEE 1212 immediate fields are 24-bit values carried in a 32-bit container (0x00XXXXXX).
    uint32_t unitSpecId{0};
    uint32_t unitSwVersion{0};

    std::optional<uint32_t> logicalUnitNumber;
    std::optional<uint32_t> modelId;
    std::optional<std::string> modelName;

    // SBP-2 specific (from Management_Agent_Offset, Unit_Characteristics, Fast_Start keys)
    std::optional<uint32_t> managementAgentOffset;
    std::optional<uint32_t> unitCharacteristics;
    std::optional<uint32_t> fastStart;
};

// ROM lifecycle state (matching Apple IOFireWireROMCache patterns)
enum class ROMState : uint8_t {
    Fresh,      // Just read in current generation
    Validated,  // Confirmed valid across bus reset (device reappeared)
    Suspended,  // From previous generation, not yet validated (bus reset occurred)
    Invalid     // Marked for removal (device disappeared or ROM changed)
};

// Parsed Config ROM (immutable snapshot per generation)
// NOTE: rawQuadlets is stored in BIG-ENDIAN wire order (byte-exact for GUI export).
struct ConfigROM {
    Generation gen{0};
    uint16_t nodeId{kInvalidNodeId};
    BusInfoBlock bib{};

    // Bounded slice of Root Directory (first N entries, typically 8-16)
    std::vector<RomEntry> rootDirMinimal;

    // Text descriptors from ROM leafs (vendor/model names)
    std::string vendorName;
    std::string modelName;

    // Parsed Unit_Directory blocks (IEEE 1212 / TA 1999027)
    std::vector<UnitDirectory> unitDirectories;

    // Raw ROM quadlets for debugging/GUI export (bounded)
    std::vector<uint32_t> rawQuadlets;

    // State management (matching Apple IOFireWireFamily patterns)
    ROMState state{ROMState::Fresh};
    Generation firstSeen{0};        // Original discovery generation
    Generation lastValidated{0};     // Last time validated after bus reset
};

// ============================================================================
// Device Classification & Lifecycle
// ============================================================================

enum class DeviceKind : uint8_t {
    Unknown,
    AV_C,                       // AV/C audio device
    TA_61883,                   // 1394 Trade Association IEC 61883
    VendorSpecificAudio,
    Storage,
    Camera
};

enum class LifeState : uint8_t {
    Discovered,    // Node seen in Self-ID
    Identified,    // ROM fetched & parsed
    Ready,         // Passed policy checks (candidate for higher layer)
    Quarantined,   // Duplicate GUID or policy violation
    Lost           // Node gone this generation
};

// ============================================================================
// Config-ROM identity evidence
// ============================================================================
//
// Raw, immutable evidence read from the Config ROM. Nothing here is promoted to
// a canonical identity: callers pick the evidence appropriate to their protocol
// family. That distinction is load-bearing rather than stylistic —
//
//   - A zero is not an absence. MOTU publishes root model_id 0, which is why
//     DeviceProtocolFactory::Create has to match that family on the unit
//     directory instead. `std::optional` says "absent"; `uint32_t{0}` cannot.
//   - A device has units, plural. The TC Applied Technologies devices publish an
//     audio unit and a MIDI unit; a single specId/version pair can only describe
//     one of them.
//   - The GUID is evidence, not a key. It can be zero, and its vendor-defined
//     bits do not have to agree with the root directory: the Focusrite Saffire
//     Pro 40 TCD3070 carries model field 0x13 in its GUID while its root/unit
//     directory says 0x0000de (Linux dice.c documents the same quirk).
//
// Ported from the `midi` branch, which restructured DeviceRecord around these
// types. Here they are added ALONGSIDE the flat fields below rather than
// replacing them, so no existing call site breaks; see the note on DeviceRecord.

struct UnitIdentityEvidence {
    uint32_t unitDirectoryOffset{0};
    std::optional<uint32_t> vendorId;
    std::optional<uint32_t> modelId;
    std::optional<uint32_t> specifierId;
    std::optional<uint32_t> version;
    std::optional<uint32_t> logicalUnitNumber;
    std::optional<std::string> vendorName;
    std::optional<std::string> modelName;
};

struct DeviceIdentityEvidence {
    Guid64 observedGuid{0};
    uint32_t nodeVendorOui{0};
    std::vector<uint32_t> rawBusInfoQuadlets;  // big-endian wire order

    std::optional<uint32_t> rootVendorId;
    std::optional<uint32_t> rootModelId;
    std::string rootVendorName;
    std::string rootModelName;

    std::vector<UnitIdentityEvidence> units;
};

/// Why a device was refused, as one neutral verdict consumers can read instead
/// of each re-deriving identity for itself.
enum class QuarantineReason : uint8_t {
    None = 0,
    ZeroObservedGuid,
    DuplicateObservedGuid,
    HazardousNoProbe,
    AmbiguousIdentity,
    InsufficientSafeEvidence,
    UnsupportedFamily,
};

/// Which AV/C command shapes a device may be sent, decided from Config-ROM
/// identity alone and stamped onto the record the same way QuarantineReason is.
enum class AvcCommandFilterId : uint8_t {
    /// No restriction. Every ordinary device.
    Unrestricted = 0,
    /// M-Audio special firmware (FireWire 1814, ProjectMix I/O), which hangs on
    /// AV/C it does not implement.
    MAudioSpecialBeBoB,
    /// Quarantined or hazardous device: block all AV/C commands.
    BlockAll,
};

// Device record anchored to GUID (stable across bus resets)
//
// MIGRATION: `identity` is the destination; the flat fields beneath it are a
// compatibility shim kept so the ~315 existing call sites continue to compile.
// Both are populated and must agree. New code reads `identity` (or the
// accessors at the bottom); subsystems move over one at a time — Audio first,
// where there is hardware to test against, SBP-2 and AV/C last — and the flat
// fields are deleted when the last caller moves.
struct DeviceRecord {
    // ---- Runtime handle ----
    // Unique within one driver lifetime and never derived from the GUID, which
    // is an observation: shipping devices report zero and duplicate EUI-64
    // values. Anything that needs to *address* a device or unit uses this;
    // `guid` below only describes one.
    DeviceInstanceId instanceId{};

    // ---- Config-ROM evidence (destination) ----
    DeviceIdentityEvidence identity{};
    QuarantineReason quarantineReason{QuarantineReason::None};
    AvcCommandFilterId avcCommandFilter{AvcCommandFilterId::Unrestricted};

    // ---- Stable identity (persistent across resets) ---- [shim, see above]
    Guid64 guid{0};
    // Device incarnation changes only when this GUID is removed and later
    // discovered again. Route epoch changes on reset/rebind independently.
    uint64_t deviceIncarnation{0};
    uint64_t routeEpoch{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
    DeviceKind kind{DeviceKind::Unknown};

    // ---- Text descriptors from ROM ----
    std::string vendorName;
    std::string modelName;

    // ---- Live mapping (current generation) ----
    Generation gen{0};
    uint16_t nodeId{kInvalidNodeId}; // 0xFFFF when not present this gen
    LinkPolicy link{};
    LifeState state{LifeState::Discovered};

    // ---- Audio classification (inferred from ROM) ----
    bool isAudioCandidate{false};    // Unit_Spec_Id==0x00A02D or AV/C Audio
    bool supportsAMDTP{false};       // Inferred from spec/version combos

    // ---- Optional metadata ----
    std::optional<uint32_t> unitSpecId;
    std::optional<uint32_t> unitSwVersion;

    // ---- Accessors over `identity` ----
    // The migration path: call sites move to these before the flat fields go,
    // so the final deletion is a compile-checked no-op rather than a rewrite.
    [[nodiscard]] Guid64 ObservedGuid() const noexcept { return identity.observedGuid; }
    [[nodiscard]] uint32_t RootVendorIdOrZero() const noexcept {
        return identity.rootVendorId.value_or(0);
    }
    [[nodiscard]] uint32_t RootModelIdOrZero() const noexcept {
        return identity.rootModelId.value_or(0);
    }
    /// First unit directory matching `specifierId`, or nullptr. A device has
    /// units plural — the flat unitSpecId/unitSwVersion pair can only describe
    /// one, which is why this exists.
    [[nodiscard]] const UnitIdentityEvidence*
    FindUnitBySpecifier(uint32_t specifierId) const noexcept {
        for (const auto& unit : identity.units) {
            if (unit.specifierId.has_value() && *unit.specifierId == specifierId) {
                return &unit;
            }
        }
        return nullptr;
    }
};

// ============================================================================
// Discovery Snapshot (published to higher layers)
// ============================================================================

struct DiscoverySnapshot {
    Generation gen{0};
    std::vector<DeviceRecord> devices;
    
    // Optional diagnostics
    std::vector<std::string> warnings;
};

// ============================================================================
// ROM Scanner Parameters
// ============================================================================

struct ROMScannerParams {
    FwSpeed startSpeed{FwSpeed::S400};
    uint8_t maxInflight{2};
    uint8_t perStepRetries{2};
    uint8_t configROMReadyRetries{4};
    uint64_t configROMReadyRetryDelayNs{500ULL * 1'000'000ULL};
    bool doIRMCheck{false};
};

} // namespace ASFW::Discovery
