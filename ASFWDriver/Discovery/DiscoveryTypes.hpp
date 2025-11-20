#pragma once

#include "DiscoveryValues.hpp"  // FwSpeed enum and constants
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ASFW::Discovery {

// ============================================================================
// Addressing & Identity
// ============================================================================

using Generation = uint16_t;
using Guid64 = uint64_t;

struct FwAddress {
    uint16_t bus{0};
    uint8_t node{0xFF};
    
    FwAddress() = default;
    FwAddress(uint16_t b, uint8_t n) : bus(b), node(n) {}
};

// ============================================================================
// Speed & Link Policy
// ============================================================================
// FwSpeed enum is now defined in DiscoveryValues.hpp

struct LinkPolicy {
    // TODO: S100 hardcoded for maximum hardware compatibility.
    FwSpeed localToNode{FwSpeed::S100};      // Negotiated/observed speed
    uint16_t maxPayloadBytes{512};           // Clamp for Async TX (depends on MaxRec, speed, policy)
    bool halvePackets{false};                // Stability escape hatch
};

// ============================================================================
// Config ROM Structure (IEEE 1394-1995 §8.3, OHCI §7.8)
// ============================================================================

// Bus Info Block (BIB) - mandatory first 5 quadlets of Config ROM
// Located at address 0xFFFFF0000400 (20 bytes)
// IEEE 1394-1995 §8.3.2: BIB[0]=header, BIB[1]="1394", BIB[2]=capabilities, BIB[3:4]=GUID
struct BusInfoBlock {
    uint8_t crcLength{0};        // Low 8 bits of BIB[0]
    uint8_t infoVersion{0};      // Bits 23:16 of BIB[0]
    uint8_t linkSpeedCode{0};    // Bits 31:28 of BIB[0]
    uint32_t vendorId{0};        // NOT from BIB! Populated from root directory (key 0x03)
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
};

struct RomEntry {
    CfgKey key;
    uint32_t value;
    uint8_t entryType{0};  // 0=immediate, 1=CSR offset, 2=leaf, 3=directory
    uint32_t leafOffsetQuadlets{0};  // Absolute ROM offset in quadlets (for leaf/dir entries)
};

// ROM lifecycle state (matching Apple IOFireWireROMCache patterns)
enum class ROMState : uint8_t {
    Fresh,      // Just read in current generation
    Validated,  // Confirmed valid across bus reset (device reappeared)
    Suspended,  // From previous generation, not yet validated (bus reset occurred)
    Invalid     // Marked for removal (device disappeared or ROM changed)
};

// Parsed Config ROM (immutable snapshot per generation)
// All quadlets are stored in HOST byte order after swapping from wire (big-endian)
struct ConfigROM {
    Generation gen{0};
    uint8_t nodeId{0xFF};
    BusInfoBlock bib{};

    // Bounded slice of Root Directory (first N entries, typically 8-16)
    std::vector<RomEntry> rootDirMinimal;

    // Text descriptors from ROM leafs (vendor/model names)
    std::string vendorName;
    std::string modelName;

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

// Device record anchored to GUID (stable across bus resets)
struct DeviceRecord {
    // ---- Stable identity (persistent across resets) ----
    Guid64 guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
    DeviceKind kind{DeviceKind::Unknown};

    // ---- Text descriptors from ROM ----
    std::string vendorName;
    std::string modelName;

    // ---- Live mapping (current generation) ----
    Generation gen{0};
    uint8_t nodeId{0xFF};        // 0xFF when not present this gen
    LinkPolicy link{};
    LifeState state{LifeState::Discovered};

    // ---- Audio classification (inferred from ROM) ----
    bool isAudioCandidate{false};    // Unit_Spec_Id==0x00A02D or AV/C Audio
    bool supportsAMDTP{false};       // Inferred from spec/version combos

    // ---- Optional metadata ----
    std::optional<uint8_t> unitSpecId;
    std::optional<uint8_t> unitSwVersion;
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
    // TODO: Investigate adaptive speed based on BiB
    FwSpeed startSpeed{FwSpeed::S100};
    uint8_t maxInflight{2};              // Limit outstanding nodes
    uint8_t perStepRetries{2};           // Before downgrading speed
    // ROM size determined dynamically from BIB crc_length field per IEEE 1212
};

} // namespace ASFW::Discovery

