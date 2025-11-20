//
// AVCUnit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Unit - wraps Discovery::FWUnit with AV/C-specific functionality
// Owns FCPTransport, provides high-level command API, caches probe results
//

#pragma once

#include <DriverKit/IOLib.h>
#include <memory>
#include <vector>
#include "FCPTransport.hpp"
#include "AVCCommands.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Async/AsyncSubsystem.hpp"

namespace ASFW::Protocols::AVC {

//==============================================================================
// AV/C Subunit Descriptor
//==============================================================================

/// AV/C Subunit representation
struct AVCSubunit {
    uint8_t type{0};        ///< Subunit type (AVCSubunitType enum)
    uint8_t id{0};          ///< Subunit ID (0-7)
    uint8_t numDestPlugs{0}; ///< Destination (input) plugs
    uint8_t numSrcPlugs{0};  ///< Source (output) plugs

    /// Get subunit address byte for CDB
    uint8_t Address() const {
        return MakeSubunitAddress(static_cast<AVCSubunitType>(type), id);
    }
};

//==============================================================================
// AV/C Unit
//==============================================================================

/// AV/C Unit - represents an AV/C-capable FireWire device
///
/// Wraps Discovery::FWUnit and provides:
/// - FCP transport instance
/// - High-level AV/C command submission API
/// - Cached probe results (subunits, plugs)
/// - Bus reset handling
///
/// **Lifecycle**:
/// - Created when AVCDiscovery detects AV/C unit (spec ID 0x00A02D)
/// - Initialize() probes subunits and plugs
/// - OnBusReset() called when topology changes
/// - Destroyed when unit terminated
///
/// **Usage**:
/// ```cpp
/// auto avcUnit = std::make_shared<AVCUnit>(device, unit, asyncSubsystem);
///
/// avcUnit->Initialize([](bool success) {
///     if (success) {
///         // Unit ready for use
///     }
/// });
///
/// // Query plugs
/// avcUnit->GetPlugInfo([](AVCResult result, const PlugInfo& info) {
///     os_log_info(..., "Plugs: %d in, %d out",
///                  info.numDestPlugs, info.numSrcPlugs);
/// });
/// ```
class AVCUnit : public std::enable_shared_from_this<AVCUnit> {
public:
    /// Plug info (unit-level)
    using PlugInfo = AVCPlugInfoCommand::PlugInfo;

    /// Constructor
    ///
    /// @param device Parent FireWire device
    /// @param unit Parent unit (must have AV/C spec ID)
    /// @param asyncSubsystem Async subsystem for FCP transport
    AVCUnit(std::shared_ptr<Discovery::FWDevice> device,
            std::shared_ptr<Discovery::FWUnit> unit,
            Async::AsyncSubsystem& asyncSubsystem);

    ~AVCUnit();

    // Non-copyable, non-movable
    AVCUnit(const AVCUnit&) = delete;
    AVCUnit& operator=(const AVCUnit&) = delete;

    /// Initialize unit (probe subunits, plugs)
    ///
    /// Queries device for:
    /// - SUBUNIT_INFO: enumerate subunits
    /// - PLUG_INFO: query unit-level plug counts
    ///
    /// @param completion Called when initialization complete (success/failure)
    void Initialize(std::function<void(bool success)> completion);

    /// Submit generic AV/C command
    ///
    /// @param cdb Command descriptor block
    /// @param completion Callback with result and response
    void SubmitCommand(const AVCCdb& cdb, AVCCompletion completion);

    /// Get unit-level plug info (async)
    ///
    /// Returns cached result if already initialized, otherwise queries device.
    ///
    /// @param completion Callback with plug info
    void GetPlugInfo(std::function<void(AVCResult, const PlugInfo&)> completion);

    /// Get subunit list (from cache)
    ///
    /// Valid after Initialize() completes successfully.
    ///
    /// @return Vector of subunit descriptors
    const std::vector<AVCSubunit>& GetSubunits() const { return subunits_; }

    /// Get underlying FWUnit
    std::shared_ptr<Discovery::FWUnit> GetFWUnit() const { return unit_.lock(); }

    /// Get underlying FWDevice
    std::shared_ptr<Discovery::FWDevice> GetDevice() const { return device_.lock(); }

    /// Get FCP transport (for PCRSpace or advanced use)
    FCPTransport& GetFCPTransport() { return *fcpTransport_; }
    const FCPTransport& GetFCPTransport() const { return *fcpTransport_; }

    /// Get async subsystem (for PCRSpace)
    Async::AsyncSubsystem& GetAsyncSubsystem() { return asyncSubsystem_; }

    /// Bus reset notification
    ///
    /// Called by AVCDiscovery when bus reset occurs.
    /// Forwards to FCPTransport and optionally invalidates cache.
    ///
    /// @param newGeneration New generation number
    void OnBusReset(uint32_t newGeneration);

    /// Check if initialized
    bool IsInitialized() const { return initialized_; }

    /// Get unit GUID
    uint64_t GetGUID() const;

    /// Get unit spec ID (should be 0x00A02D for AV/C)
    uint32_t GetSpecID() const;

private:
    /// Probe subunits (SUBUNIT_INFO command)
    void ProbeSubunits(std::function<void(bool)> completion);

    /// Probe unit-level plugs (PLUG_INFO command)
    void ProbePlugs(std::function<void(bool)> completion);

    /// Parse subunit info and store in cache
    void StoreSubunitInfo(const AVCSubunitInfoCommand::SubunitInfo& info);

    // Parent device and unit (weak pointers to avoid cycles)
    std::weak_ptr<Discovery::FWDevice> device_;
    std::weak_ptr<Discovery::FWUnit> unit_;

    // Async subsystem reference
    Async::AsyncSubsystem& asyncSubsystem_;

    // FCP transport (owned)
    std::unique_ptr<FCPTransport> fcpTransport_;

    // Cached probe results
    std::vector<AVCSubunit> subunits_;
    PlugInfo unitPlugInfo_;

    // Initialization state
    bool initialized_{false};

    os_log_t log_{OS_LOG_DEFAULT};
};

} // namespace ASFW::Protocols::AVC
