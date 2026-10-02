//
// AVCUnit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Unit implementation
//

#include "AVCUnit.hpp"
#include "Graph/AvcGraphBuilder.hpp"
#include "Graph/DiscoveryGraph.hpp"
#include "Graph/AvcStreamGeometry.hpp"
#include <algorithm>
#include "../../Common/CallbackUtils.hpp"
#include "../../Logging/Logging.hpp"
#include "Commands/GeneralCommands.hpp"
#include "Commands/SignalSourceCommand.hpp"
#include "Commands/StreamFormatCommand.hpp"
#include "Core/RateCodes.hpp"
#include "Music/MusicSubunit.hpp"
#include "Audio/AudioSubunit.hpp"

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCUnit::AVCUnit(std::shared_ptr<Discovery::FWDevice> device,
                 std::shared_ptr<Discovery::FWUnit> unit,
                 Discovery::DeviceRegistry& routeRegistry,
                 Protocols::Ports::FireWireBusOps& busOps,
                 Protocols::Ports::FireWireBusInfo& busInfo,
                 Scheduling::ITimerScheduler& timerScheduler,
                 DiscoveryOptions options)
    : device_(device),
      unit_(unit),
      routeRegistry_(routeRegistry),
      busOps_(busOps),
      busInfo_(busInfo),
      timerScheduler_(timerScheduler),
      options_(options) {
    SetStreamFormatOpcodePolicy(options_.streamFormatOpcode);

    // Check for custom FCP addresses in Config ROM (optional)
    // For now, use standard addresses
    FCPTransportConfig config;
    config.commandAddress = kFCPCommandAddress;
    config.responseAddress = kFCPResponseAddress;
    config.timeoutMs = kFCPTimeoutInitial;
    config.interimTimeoutMs = kFCPTimeoutAfterInterim;
    config.maxRetries = kFCPMaxRetries;
    config.allowBusResetRetry = false;  // Default: generation-locked

    // The allowlist for firmware that hangs on unimplemented AV/C. Empty for
    // every ordinary device, which is unrestricted. Decided from Config ROM
    // before this transport exists, so the very first frame is already bounded.
    config.permittedFrames =
        PermittedFramesFor(device ? device->GetAvcCommandFilter()
                                  : Discovery::AvcCommandFilterId::Unrestricted);

    // Create FCP transport
    fcpTransport_ = std::make_shared<FCPTransport>();
    if (fcpTransport_) {
        if (!fcpTransport_->init(&busOps_, &busInfo_, device.get(), routeRegistry_, timerScheduler_, config)) {
            ASFW_LOG_ERROR(AVC, "AVCUnit: Failed to initialize FCPTransport");
            fcpTransport_.reset();
            return;
        }
        // Family code that talks to the transport directly asks with the same opcode.
        fcpTransport_->SetStreamFormatOpcodePolicy(options_.streamFormatOpcode);
    } else {
        ASFW_LOG_V1(AVC, "AVCUnit: Failed to allocate FCPTransport");
    }

    ASFW_LOG_V1(AVC,
                "AVCUnit: Created for device GUID=%llx, specID=0x%06x",
                GetGUID(), GetSpecID());
}

void AVCUnit::Submit(const ASFW::AVC::CommandFrame& frame,
                     FW::Generation generation,
                     ResponseCallback completion) {
    // Route admission here; the transport is the single allowlist admission
    // point (its filter is fixed from Config ROM before the first frame).
    const auto route = CurrentRoute();
    if (!route || route->generation != generation || !IsCurrentRoute(*route)) {
        completion(ASFW::AVC::Fail(ASFW::AVC::AvcErrorKind::kRefused)); return;
    }
    if (fcpTransport_) {
        fcpTransport_->Submit(frame, generation, std::move(completion));
    } else {
        completion(std::unexpected(ASFW::AVC::AvcError::Of(ASFW::AVC::AvcErrorKind::kTransportError)));
    }
}

ASFW::FW::NodeId AVCUnit::NodeId() const noexcept {
    return fcpTransport_ ? fcpTransport_->NodeId() : ASFW::FW::NodeId{0};
}

ASFW::FW::Generation AVCUnit::CurrentGeneration() const noexcept {
    return fcpTransport_ ? fcpTransport_->CurrentGeneration() : ASFW::FW::Generation{0};
}

uint64_t AVCUnit::Guid() const noexcept {
    return fcpTransport_ ? fcpTransport_->Guid() : GetGUID();
}

std::optional<ASFW::Discovery::DeviceRouteToken> AVCUnit::CurrentRoute() const noexcept {
    return routeRegistry_.CurrentRoute(Guid());
}

bool AVCUnit::IsCurrentRoute(const Discovery::DeviceRouteToken& route) const noexcept {
    return routeRegistry_.IsCurrent(route);
}

AVCUnit::~AVCUnit() {
    // Retire every LiveRef to this unit first: a discovery completion reached
    // from Shutdown() must not touch a unit that is being destroyed.
    RetireLifetime();
    Shutdown();
    ASFW_LOG_V1(AVC, "AVCUnit: Destroyed (GUID=%llx)", GetGUID());
}

void AVCUnit::Shutdown() {
    namespace E = ASFW::AVC::DiscoveryEngine;
    if (auto* running = std::get_if<E::RunningSlot>(&sessionSlot_)) {
        auto session = running->session;
        sessionSlot_ = E::CancellingSlot{session};
        session->RouteLost();
    }
    initialized_ = false;
    if (fcpTransport_) {
        fcpTransport_->Shutdown();
    }
}

//==============================================================================
// Initialization
//==============================================================================

void AVCUnit::Initialize(std::function<void(bool)> completion) {
    if (!TryBeginRescan()) {
        if (completion) completion(false);
        return;
    }
    InitializeAlreadyBegun(std::move(completion));
}

void AVCUnit::InitializeAlreadyBegun(std::function<void(bool)> completion) {
    namespace E = ASFW::AVC::DiscoveryEngine;
    if (!std::holds_alternative<E::IdleSlot>(sessionSlot_)) {
        if (completion) completion(false);
        return;
    }
    const Common::LiveRef<AVCUnit> live{*this};
    auto session = E::Session::Create(*this, E::SessionId{++nextSession_},
        [live, completion = std::move(completion)](E::SnapshotLease snapshot) mutable {
            auto* unit = live.Get();
            if (!unit) { if (completion) completion(false); return; }
            const bool current = snapshot && unit->IsCurrentRoute(snapshot->route);
            const bool success = current && snapshot->complete;
            if (success) {
                unit->ApplySnapshot(*snapshot);
                unit->snapshot_ = std::move(snapshot);
            }
            if (!success && unit->snapshot_ && unit->IsCurrentRoute(unit->snapshot_->route))
                unit->ApplySnapshot(*unit->snapshot_);
            unit->initialized_ = success;
            unit->sessionSlot_ = E::IdleSlot{};
            unit->FinishExternalRescan(success);
            if (completion) completion(success);
        },
        [live](E::SnapshotLease snapshot, std::function<void(E::ExtensionFacts)> done) {
            auto* unit = live.Get();
            if (!unit || snapshot->terminalError || !unit->IsCurrentRoute(snapshot->route)) { done({}); return; }
            if (unit->options_.extensionInventory) unit->options_.extensionInventory(*unit, std::move(done));
            else done({});
        });
    sessionSlot_ = E::RunningSlot{session};
    session->Start();
}

void AVCUnit::ReScan(std::function<void(bool)> completion) {
    ASFW_LOG_V1(AVC, "AVCUnit: Re-scan requested (GUID=%llx)", GetGUID());
    if (!TryBeginRescan()) {
        if (completion) completion(false);
        return;
    }
    ReScanAlreadyBegun(std::move(completion));
}

void AVCUnit::ReScanAlreadyBegun(std::function<void(bool)> completion) {
    // Retain the previous committed snapshot while replacement discovery runs.
    InitializeAlreadyBegun(std::move(completion));
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void AVCUnit::OnBusReset(uint32_t newGeneration) {
    ASFW_LOG_V2(AVC,
                "AVCUnit: Bus reset (generation %u)",
                newGeneration);

    namespace E = ASFW::AVC::DiscoveryEngine;
    if (auto* running = std::get_if<E::RunningSlot>(&sessionSlot_)) {
        auto session = running->session;
        sessionSlot_ = E::CancellingSlot{session};
        session->RouteLost();
    }
    // Forward to FCP transport (will handle pending commands)
    if (fcpTransport_) {
        fcpTransport_->OnBusReset(newGeneration);
    }
    model_.identity = Identity();

    // v1: Keep cached state (subunits, plugs rarely change)
    // Caller can re-Initialize() if topology changed

    // v2 improvement: Could invalidate cache on topology change
    // and re-probe automatically
}

void AVCUnit::OnRouteRevalidated() {
    const auto route = routeRegistry_.CurrentRoute(GetGUID());
    if (fcpTransport_ && route.has_value()) {
        fcpTransport_->OnRouteRevalidated(*route);
    }
    model_.identity = Identity();
}

//==============================================================================
// Accessors
//==============================================================================

uint64_t AVCUnit::GetGUID() const {
    auto device = device_.lock();
    if (!device) {
        return 0;
    }
    return device->GetGUID();
}

uint32_t AVCUnit::GetSpecID() const {
    auto unit = unit_.lock();
    if (!unit) {
        return 0;
    }
    return unit->GetUnitSpecID();
}

void AVCUnit::ApplySnapshot(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot) {
    model_ = snapshot.unit;
    subunits_.clear(); descriptorInfo_ = {};
    for (const auto& sub : snapshot.unit.subunits) {
        const auto type = static_cast<AVCSubunitType>(sub.id.type);
        std::shared_ptr<Subunit> projection;
        if (sub.id.type == ASFW::AVC::SubunitType::kMusic) {
            auto music = std::make_shared<Music::MusicSubunit>(type, sub.id.id);
            music->LoadSnapshot(snapshot); projection = std::move(music);
        } else if (sub.id.type == ASFW::AVC::SubunitType::kAudio) {
            auto audio = std::make_shared<Audio::AudioSubunit>(type, sub.id.id);
            audio->LoadSnapshot(snapshot); projection = std::move(audio);
        } else {
            class InventorySubunit final : public Subunit {
            public:
                InventorySubunit(AVCSubunitType type, uint8_t id) : Subunit(type, id) {}
                std::string GetName() const override { return "Generic"; }
            };
            projection = std::make_shared<InventorySubunit>(type, sub.id.id);
        }
        projection->SetPlugCounts({sub.plugs.destinationPlugs, sub.plugs.sourcePlugs});
        subunits_.push_back(std::move(projection));
    }
    for (const auto& blob : snapshot.descriptors)
        if (!blob.primaryError && !blob.bytes.empty()) descriptorInfo_.descriptorMechanismSupported = true;
    std::string name;
    if (auto device = device_.lock()) name = std::string(device->GetModelName());
    discoveredGraph_ = std::make_shared<const Graph::DeviceGraph>(Graph::BuildDiscoveryGraph(snapshot, std::move(name)));
}
