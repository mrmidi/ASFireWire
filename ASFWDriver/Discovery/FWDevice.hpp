#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include "DiscoveryTypes.hpp"
#include "FWUnit.hpp"

namespace ASFW::Discovery {

// Forward declarations
class FWUnit;

/**
 * @brief Represents a FireWire device with lifecycle management.
 *
 * Analogous to Apple's IOFireWireDevice. Wraps DeviceRecord with added
 * unit management and lifecycle state machine.
 *
 * Lifecycle:
 * - Created when device first discovered and ROM parsed
 * - Ready when passed policy checks and units published
 * - Suspended when device lost after bus reset (persists in registry)
 * - Resumed when device reappears with matching GUID
 * - Terminated when permanently removed from bus
 *
 * Units:
 * - Parsed from Config ROM unit directories
 * - Each unit published independently for spec-based matching
 * - Unit lifecycle tied to parent device lifecycle
 */
class FWDevice : public std::enable_shared_from_this<FWDevice> {
public:
    /**
     * @brief Device state across bus resets and lifecycle.
     */
    enum class State {
        Created,      // Just created from ROM parse
        Ready,        // Units published, available for use
        Suspended,    // Lost after bus reset (not in current topology)
        Terminated    // Permanently removed
    };

    /**
     * @brief Create device from parsed ROM and device record.
     *
     * @param record Device record from DeviceRegistry
     * @param rom Parsed Config ROM with unit directories
     * @return Shared pointer to new device, or nullptr if creation failed
     */
    static std::shared_ptr<FWDevice> Create(
        const DeviceRecord& record,
        const ConfigROM& rom
    );

    // === Identity (Immutable, from ROM) ===

    /**
     * @brief Get device GUID (stable across bus resets).
     */
    Guid64 GetGUID() const { return guid_; }

    /**
     * @brief Get Vendor ID (IEEE 1212 key 0x03).
     */
    uint32_t GetVendorID() const { return vendorId_; }

    /**
     * @brief Get Model ID (IEEE 1212 key 0x17).
     */
    uint32_t GetModelID() const { return modelId_; }

    /**
     * @brief Get device kind classification.
     */
    DeviceKind GetKind() const { return kind_; }

    // === Text Descriptors ===

    std::string_view GetVendorName() const { return vendorName_; }
    std::string_view GetModelName() const { return modelName_; }

    // === Current Generation Info (Updated on bus reset) ===

    /**
     * @brief Get current generation number.
     */
    Generation GetGeneration() const { return generation_; }

    /**
     * @brief Get current node ID (valid only for current generation).
     * @return Node ID, or 0xFF if device not present
     */
    uint8_t GetNodeID() const { return nodeId_; }

    /**
     * @brief Get link policy for communication with this device.
     */
    const LinkPolicy& GetLinkPolicy() const { return linkPolicy_; }

    // === Unit Management ===

    /**
     * @brief Get all units for this device.
     * @return Vector of unit shared pointers
     */
    const std::vector<std::shared_ptr<FWUnit>>& GetUnits() const { return units_; }

    /**
     * @brief Find units matching spec ID and optional SW version.
     *
     * @param specId Required Unit_Spec_ID
     * @param swVersion Optional Unit_SW_Version (matches any if not provided)
     * @return Vector of matching units
     */
    std::vector<std::shared_ptr<FWUnit>> FindUnitsBySpec(
        uint32_t specId,
        std::optional<uint32_t> swVersion = {}
    ) const;

    // === Audio Classification ===

    bool IsAudioCandidate() const { return isAudioCandidate_; }
    bool SupportsAMDTP() const { return supportsAMDTP_; }

    // === State Management ===

    State GetState() const { return state_; }
    bool IsReady() const { return state_ == State::Ready; }
    bool IsSuspended() const { return state_ == State::Suspended; }
    bool IsTerminated() const { return state_ == State::Terminated; }

    // === Lifecycle Methods ===

    /**
     * @brief Publish device and all units (transition Created → Ready).
     * Makes device and units available for discovery.
     */
    void Publish();

    /**
     * @brief Suspend device (transition Ready → Suspended).
     * Called when device lost after bus reset. Device persists in registry
     * but is unavailable until resumed. All units suspended.
     */
    void Suspend();

    /**
     * @brief Resume device (transition Suspended → Ready).
     * Called when device reappears after bus reset with matching GUID.
     * Updates generation/nodeId and resumes all units.
     *
     * @param newGen New generation number
     * @param newNodeId New node ID
     * @param newLink New link policy
     */
    void Resume(Generation newGen, uint8_t newNodeId, const LinkPolicy& newLink);

    /**
     * @brief Terminate device (transition * → Terminated).
     * Permanent removal. Terminates all units. Called when device
     * removed from bus or driver stopping.
     */
    void Terminate();

private:
    // Private constructor - use Create() factory
    FWDevice(const DeviceRecord& record);

    // Parse unit directories from ROM and create FWUnit objects
    void ParseUnits(const ConfigROM& rom);

    // Extract unit directory entries from ROM at given offset
    std::vector<RomEntry> ExtractUnitDirectory(
        const ConfigROM& rom,
        uint32_t offsetQuadlets
    ) const;

    // === Immutable Identity ===
    const Guid64 guid_;
    const uint32_t vendorId_;
    const uint32_t modelId_;
    const DeviceKind kind_;

    // Text descriptors
    std::string vendorName_;
    std::string modelName_;

    // Audio classification
    bool isAudioCandidate_{false};
    bool supportsAMDTP_{false};

    // === Mutable State (Updated on bus reset) ===
    Generation generation_{0};
    uint8_t nodeId_{0xFF};
    LinkPolicy linkPolicy_{};
    State state_{State::Created};

    // === Unit Management ===
    std::vector<std::shared_ptr<FWUnit>> units_;
};

} // namespace ASFW::Discovery
