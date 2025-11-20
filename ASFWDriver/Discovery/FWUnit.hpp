#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Forward declaration
class FWDevice;

/**
 * @brief Represents a unit directory within a FireWire device.
 *
 * Analogous to Apple's IOFireWireUnit. Each unit represents a functional
 * capability within a device (e.g., audio interface, video capture).
 * Units are published independently and can be discovered by spec ID.
 *
 * Lifecycle:
 * - Created when parent FWDevice parses unit directories from Config ROM
 * - Lives as long as parent device exists and is Ready
 * - Suspended when parent device suspended (bus reset, device lost)
 * - Resumed when parent device reappears
 * - Terminated when parent device terminated
 */
class FWUnit : public std::enable_shared_from_this<FWUnit> {
public:
    /**
     * @brief Unit state across bus resets and lifecycle.
     */
    enum class State {
        Created,      // Just created, not yet published
        Ready,        // Published and available for use
        Suspended,    // Parent device suspended (not in current topology)
        Terminated    // Permanently removed
    };

    /**
     * @brief Create a unit from parsed directory entries.
     *
     * @param parentDevice Parent device (must outlive unit or use shared_ptr)
     * @param directoryOffset Offset in ROM (quadlets) where unit directory starts
     * @param entries Parsed ROM entries for this unit directory
     * @return Shared pointer to new unit, or nullptr if parsing failed
     */
    static std::shared_ptr<FWUnit> Create(
        std::shared_ptr<FWDevice> parentDevice,
        uint32_t directoryOffset,
        const std::vector<RomEntry>& entries
    );

    // === Identity (Immutable, from ROM) ===

    /**
     * @brief Get Unit_Spec_ID (IEEE 1212 key 0x12).
     * Identifies the specification this unit implements (e.g., 0x00A02D for 1394 TA Audio).
     */
    uint32_t GetUnitSpecID() const { return unitSpecId_; }

    /**
     * @brief Get Unit_SW_Version (IEEE 1212 key 0x13).
     * Software version of the unit specification.
     */
    uint32_t GetUnitSwVersion() const { return unitSwVersion_; }

    /**
     * @brief Get Model_ID (IEEE 1212 key 0x17, optional).
     */
    uint32_t GetModelID() const { return modelId_; }

    /**
     * @brief Get Logical_Unit_Number (IEEE 1212 key 0x14, optional).
     */
    std::optional<uint32_t> GetLUN() const { return logicalUnitNumber_; }

    /**
     * @brief Get ROM offset where this unit directory starts (debugging).
     */
    uint32_t GetDirectoryOffset() const { return directoryOffset_; }

    // === Text Descriptors (Optional, from text leaves) ===

    std::string_view GetVendorName() const { return vendorName_; }
    std::string_view GetProductName() const { return productName_; }

    // === Parent Device ===

    /**
     * @brief Get parent device.
     * @return Shared pointer to device, or nullptr if device destroyed.
     */
    std::shared_ptr<FWDevice> GetDevice() const;

    // === State Management ===

    State GetState() const { return state_; }
    bool IsReady() const { return state_ == State::Ready; }
    bool IsSuspended() const { return state_ == State::Suspended; }
    bool IsTerminated() const { return state_ == State::Terminated; }

    /**
     * @brief Check if unit matches spec/version criteria.
     *
     * @param specId Required Unit_Spec_ID
     * @param swVersion Optional Unit_SW_Version (matches any if not provided)
     * @return true if unit matches criteria
     */
    bool Matches(uint32_t specId, std::optional<uint32_t> swVersion = {}) const;

    // === Lifecycle Methods (called by parent device) ===

    /**
     * @brief Publish unit (transition Created → Ready).
     * Makes unit available for discovery. Called by parent device after
     * unit fully constructed.
     */
    void Publish();

    /**
     * @brief Suspend unit (transition Ready → Suspended).
     * Called when parent device lost after bus reset. Unit persists
     * but is unavailable until resumed.
     */
    void Suspend();

    /**
     * @brief Resume unit (transition Suspended → Ready).
     * Called when parent device reappears after bus reset.
     */
    void Resume();

    /**
     * @brief Terminate unit (transition * → Terminated).
     * Permanent removal. Called when parent device terminated or
     * unit explicitly removed.
     */
    void Terminate();

private:
    // Private constructor - use Create() factory
    FWUnit(std::shared_ptr<FWDevice> parentDevice, uint32_t directoryOffset);

    // Parse unit directory entries to extract keys
    void ParseEntries(const std::vector<RomEntry>& entries);

    // Extract text descriptors from text leaf entries
    void ExtractTextLeaves(const std::vector<RomEntry>& entries);

    // Parent device (strong reference - unit keeps device alive)
    std::shared_ptr<FWDevice> parentDevice_;

    // ROM location
    const uint32_t directoryOffset_;

    // Matching keys (IEEE 1212 unit directory)
    uint32_t unitSpecId_{0};       // Key 0x12 - Required
    uint32_t unitSwVersion_{0};    // Key 0x13 - Required
    uint32_t modelId_{0};          // Key 0x17 - Optional
    std::optional<uint32_t> logicalUnitNumber_;  // Key 0x14 - Optional

    // Text descriptors (from text leaves)
    std::string vendorName_;
    std::string productName_;

    // State
    State state_{State::Created};
};

} // namespace ASFW::Discovery
