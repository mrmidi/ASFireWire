//
// AVCUnit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Unit - wraps Discovery::FWUnit with AV/C-specific functionality
// Owns FCPTransport, provides high-level command API, caches probe results
//

#pragma once

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#endif
#include <memory>
#include <atomic>
#include <span>
#include <vector>
#include "FCPTransport.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"
#include "Core/AvcUnitModel.hpp"
#include "Core/IAvcUnit.hpp"
#include "Discovery/DiscoverySession.hpp"

namespace ASFW::Protocols::AVC {

class AVCDiscovery;

enum class AVCDiscoveryStatus : uint8_t {
    Idle = 0, Running = 1, Completed = 2, Failed = 3, Skipped = 4,
    /// A manual refresh was refused because the device's audio is active.
    BlockedByAudio = 5
};

//==============================================================================
// Forward Declarations
//==============================================================================
class DescriptorAccessor;
namespace Graph { struct DeviceGraph; struct StreamGraph; }

//==============================================================================
// AV/C Unit
//==============================================================================

class AVCUnit;

// What discovery runs beyond the standard AV/C commands. The unit's owner sets
// it from the catalog plan; the unit itself never reads the catalog.
struct AVCUnitDiscoveryOptions {
    // The chip's read-only extension inventory (BridgeCo, Oxford). Runs after
    // the generic discovery and before the discovery status completes, at
    // attach and on every refresh; it must call `done` exactly once.
    // It reaches the unit only through IAvcUnit and holds it by LiveRef, never
    // by ownership: replay runs the same inventory against a recorded unit, and
    // a destroyed unit ends it silently (done is then never called, because
    // the session that would receive it was destroyed with the unit).
    // `discovered` is what generic discovery found so far; the inventory reads
    // it instead of asking the device again.
    std::function<void(ASFW::AVC::IAvcUnit& unit, ASFW::AVC::DiscoveryEngine::SnapshotLease discovered,
                       std::function<void(ASFW::AVC::DiscoveryEngine::ExtensionFacts)> done)> extensionInventory;
    // Which stream-format opcode the chip is asked with (BridgeCo: 0x2F only).
    ASFW::AVC::IAvcUnit::StreamFormatOpcodePolicy streamFormatOpcode{
        ASFW::AVC::IAvcUnit::StreamFormatOpcodePolicy::kLearn};
};

class AVCUnit : public std::enable_shared_from_this<AVCUnit>,
                public ASFW::AVC::IAvcUnit {
public:
    using DiscoveryOptions = AVCUnitDiscoveryOptions;

    AVCUnit(std::shared_ptr<Discovery::FWDevice> device,
            std::shared_ptr<Discovery::FWUnit> unit,
            Discovery::DeviceRegistry& routeRegistry,
            Protocols::Ports::FireWireBusOps& busOps,
            Protocols::Ports::FireWireBusInfo& busInfo,
            Scheduling::ITimerScheduler& timerScheduler,
            DiscoveryOptions options = {});

    ~AVCUnit() override;

    AVCUnit(const AVCUnit&) = delete("a unit owns its FCP transport and discovery session; there is one per device");
    AVCUnit& operator=(const AVCUnit&) = delete("a unit owns its FCP transport and discovery session; there is one per device");

    // --- IAvcUnit implementation ---
    void Submit(const ASFW::AVC::CommandFrame& frame,
                FW::Generation generation,
                ResponseCallback completion) override;

    [[nodiscard]] FW::NodeId NodeId() const noexcept override;
    [[nodiscard]] FW::Generation CurrentGeneration() const noexcept override;
    [[nodiscard]] uint64_t Guid() const noexcept override;

    [[nodiscard]] std::optional<Discovery::DeviceRouteToken> CurrentRoute() const noexcept override;
    [[nodiscard]] bool IsCurrentRoute(const Discovery::DeviceRouteToken& route) const noexcept override;

    void Initialize(std::function<void(bool success)> completion);

    void ReScan(std::function<void(bool success)> completion);
    [[nodiscard]] AVCDiscoveryStatus GetDiscoveryStatus() const noexcept {
        return discoveryStatus_.load(std::memory_order_acquire);
    }
    void MarkRescanBlockedByAudio() noexcept {
        if (rescanInProgress_.load(std::memory_order_acquire)) return;
        auto status = discoveryStatus_.load(std::memory_order_acquire);
        while (status != AVCDiscoveryStatus::Running &&
               !discoveryStatus_.compare_exchange_weak(status, AVCDiscoveryStatus::BlockedByAudio,
                                                       std::memory_order_acq_rel)) {}
    }
    void MarkRescanSkipped() noexcept {
        if (rescanInProgress_.load(std::memory_order_acquire)) return;
        auto status = discoveryStatus_.load(std::memory_order_acquire);
        while (status != AVCDiscoveryStatus::Running &&
               !discoveryStatus_.compare_exchange_weak(status, AVCDiscoveryStatus::Skipped,
                                                       std::memory_order_acq_rel)) {}
    }
    [[nodiscard]] bool TryBeginRescan() noexcept {
        bool expected = false;
        if (!rescanInProgress_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        discoveryStatus_.store(AVCDiscoveryStatus::Running, std::memory_order_release);
        return true;
    }
    /// End a rescan begun with TryBeginRescan() that a family bring-up ran
    /// instead of Initialize (BeBoB plug probes).
    void FinishExternalRescan(bool success) noexcept {
        discoveryStatus_.store(success ? AVCDiscoveryStatus::Completed : AVCDiscoveryStatus::Failed,
                               std::memory_order_release);
        rescanInProgress_.store(false, std::memory_order_release);
    }
    /// Start a new exchange log for this unit (manual refresh).
    void BeginExchangeSession() { if (fcpTransport_) fcpTransport_->BeginExchangeSession(); }
    /// Every FCP exchange with this unit since the session started.
    [[nodiscard]] FcpExchangeLog CopyExchangeLog() const {
        return fcpTransport_ ? fcpTransport_->CopyExchangeLog() : FcpExchangeLog{};
    }
    std::shared_ptr<const Graph::DeviceGraph> GetDiscoveredGraph() const noexcept { return discoveredGraph_; }
    void RememberConfirmedDuplexRate(const Discovery::DeviceRouteToken& route, uint32_t rateHz) override;
    [[nodiscard]] bool HasUserFeaturePreference(uint8_t subunit, uint8_t block) const noexcept override;
    void RememberConfirmedFeature(const Discovery::DeviceRouteToken& route, uint8_t subunit,
                                  const ASFW::AVC::Cmd::FeatureReply& reply);

    /// The last committed discovery: unit, subunits, plugs, descriptors,
    /// controls. Immutable; every consumer reads discovered facts from here.
    [[nodiscard]] ASFW::AVC::DiscoveryEngine::SnapshotLease GetDiscoverySnapshot() const noexcept { return snapshot_; }



    std::shared_ptr<Discovery::FWUnit> GetFWUnit() const { return unit_.lock(); }

    std::shared_ptr<Discovery::FWDevice> GetDevice() const { return device_.lock(); }

    std::shared_ptr<FCPTransport> GetFCPTransportShared() const { return fcpTransport_; }

    void OnBusReset(uint32_t newGeneration);
    void OnRouteRevalidated();

    /// Stop the unit's FCP transport before the async bus is torn down.
    void Shutdown();

    bool IsInitialized() const { return initialized_; }

    uint64_t GetGUID() const;

    uint32_t GetSpecID() const;

private:
    friend class AVCDiscovery;
    void InitializeAlreadyBegun(std::function<void(bool success)> completion);
    void ReScanAlreadyBegun(std::function<void(bool success)> completion);

    void ApplySnapshot(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot);
    /// Writes everything the snapshot found to the driver ring, by spec name (Discovery/DiscoveryLog.hpp).
    void LogDiscovery(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot) const;
    std::weak_ptr<Discovery::FWDevice> device_;
    std::weak_ptr<Discovery::FWUnit> unit_;
    Discovery::DeviceRegistry& routeRegistry_;

    Protocols::Ports::FireWireBusOps& busOps_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    Scheduling::ITimerScheduler& timerScheduler_;
    const DiscoveryOptions options_;

    std::shared_ptr<FCPTransport> fcpTransport_;

    std::shared_ptr<const Graph::DeviceGraph> discoveredGraph_{};

    ASFW::AVC::DiscoveryEngine::SessionSlot sessionSlot_{ASFW::AVC::DiscoveryEngine::IdleSlot{}};
    ASFW::AVC::DiscoveryEngine::SnapshotLease snapshot_;
    uint64_t nextSession_{0};
    bool initialized_{false};
    std::atomic<AVCDiscoveryStatus> discoveryStatus_{AVCDiscoveryStatus::Idle};
    std::atomic<bool> rescanInProgress_{false};
};

} // namespace ASFW::Protocols::AVC
