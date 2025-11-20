//
// AVCDiscovery.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery - auto-detects AV/C units and creates AVCUnit instances
// Implements IUnitObserver for lifecycle notifications
//

#pragma once

#include <DriverKit/IOLib.h>
#include <memory>
#include <unordered_map>
#include "AVCUnit.hpp"
#include "../../Discovery/IDeviceManager.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Async/AsyncSubsystem.hpp"

namespace ASFW::Protocols::AVC {

//==============================================================================
// AV/C Discovery
//==============================================================================

/// AV/C Discovery - creates AVCUnit for AV/C-capable devices
///
/// Observes unit lifecycle events from DeviceManager and creates AVCUnit
/// instances for units with AV/C spec ID (0x00A02D).
///
/// **Integration**:
/// ```cpp
/// // In your driver initialization:
/// avcDiscovery_ = std::make_unique<AVCDiscovery>(deviceManager_, asyncSubsystem_);
///
/// // Later, when device is AV/C-capable:
/// AVCUnit* avcUnit = avcDiscovery_->GetAVCUnit(unitGuid);
/// if (avcUnit) {
///     avcUnit->GetPlugInfo([](AVCResult result, const PlugInfo& info) {
///         // ...
///     });
/// }
/// ```
///
/// **Thread Safety**:
/// - All public methods are thread-safe (protected by IOLock)
/// - Observer callbacks invoked with lock held
/// - AVCUnit operations happen outside lock
class AVCDiscovery : public Discovery::IUnitObserver {
public:
    /// Constructor
    ///
    /// @param deviceManager Device manager for observation
    /// @param asyncSubsystem Async subsystem for FCP transport
    AVCDiscovery(Discovery::IDeviceManager& deviceManager,
                 Async::AsyncSubsystem& asyncSubsystem);

    ~AVCDiscovery() override;

    // Non-copyable, non-movable
    AVCDiscovery(const AVCDiscovery&) = delete;
    AVCDiscovery& operator=(const AVCDiscovery&) = delete;

    //==========================================================================
    // IUnitObserver Interface
    //==========================================================================

    void OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) override;

    //==========================================================================
    // Public API
    //==========================================================================

    /// Get AVCUnit for unit GUID
    ///
    /// @param guid Unit GUID (from FWUnit::GetGUID())
    /// @return Pointer to AVCUnit, or nullptr if not AV/C or not found
    AVCUnit* GetAVCUnit(uint64_t guid);

    /// Get AVCUnit for FWUnit
    ///
    /// @param unit FWUnit to query
    /// @return Pointer to AVCUnit, or nullptr if not AV/C or not found
    AVCUnit* GetAVCUnit(std::shared_ptr<Discovery::FWUnit> unit);

    /// Get all AV/C units
    ///
    /// @return Vector of pointers to AVCUnit instances
    std::vector<AVCUnit*> GetAllAVCUnits();

    /// Get FCP transport for node ID (for PacketRouter integration)
    ///
    /// Maps node ID → FCPTransport for routing FCP responses.
    ///
    /// @param nodeID Node ID from packet source
    /// @return Pointer to FCPTransport, or nullptr if not found
    FCPTransport* GetFCPTransportForNodeID(uint16_t nodeID);

    /// Update node ID mappings after bus reset
    ///
    /// Called when bus reset occurs and node IDs change.
    /// Rebuilds fcpTransportsByNodeID_ map from current device state.
    ///
    /// @param newGeneration New generation number
    void OnBusReset(uint32_t newGeneration);

private:
    /// Check if unit is AV/C-capable (spec ID 0x00A02D)
    ///
    /// @param unit Unit to check
    /// @return true if unit implements AV/C protocol
    bool IsAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) const;

    /// Get unit GUID (helper for weak_ptr safety)
    ///
    /// @param unit Unit to query
    /// @return GUID, or 0 if unit invalid
    uint64_t GetUnitGUID(std::shared_ptr<Discovery::FWUnit> unit) const;

    /// Rebuild node ID → FCPTransport mapping
    ///
    /// Called after bus reset when node IDs change.
    void RebuildNodeIDMap();

    Discovery::IDeviceManager& deviceManager_;
    Async::AsyncSubsystem& asyncSubsystem_;

    /// Lock protecting units_ and fcpTransportsByNodeID_
    IOLock* lock_{nullptr};

    /// Map: Unit GUID → AVCUnit instance
    std::unordered_map<uint64_t, std::shared_ptr<AVCUnit>> units_;

    /// Map: Node ID → FCPTransport (for FCP response routing)
    /// Updated on bus reset when node IDs change
    std::unordered_map<uint16_t, FCPTransport*> fcpTransportsByNodeID_;

    os_log_t log_{OS_LOG_DEFAULT};
};

} // namespace ASFW::Protocols::AVC
