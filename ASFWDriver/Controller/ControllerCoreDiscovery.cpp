#include "ControllerCore.hpp"

#include <DriverKit/IOLib.h>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "../Async/DMAMemoryImpl.hpp"
#include "../Async/FireWireBusImpl.hpp"
#include "../Async/Interfaces/IFireWireBusOps.hpp"
#include "../Bus/BusResetCoordinator.hpp"
#include "../Bus/SelfIDCapture.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../Bus/CSR/TopologyMapService.hpp"
#include "../Bus/BusManager/BusManagerElectionDriver.hpp"
#include "../Bus/BusManager/BusManagerPolicyCoordinator.hpp"
#include "../Bus/BusManager/CyclePolicyCoordinator.hpp"
#include "../Bus/BusManager/RootSelectionCoordinator.hpp"
#include "../Bus/Timing/PostResetTimingCoordinator.hpp"
#include "../Bus/IRM/IRMFallbackCoordinator.hpp"
#include "../ConfigROM/ConfigROMBuilder.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ConfigROMStore.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Common/CSRSpace.hpp"
#include "../Diagnostics/DiagnosticLogger.hpp"
#include "../Diagnostics/MetricsSink.hpp"
#include "../Discovery/DiscoveryConvergence.hpp"
#include "../Discovery/DeviceManager.hpp"
#include "../Discovery/DeviceRegistry.hpp"
#include "../Discovery/SpeedPolicy.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Hardware/IEEE1394.hpp"
#include "../Hardware/InterruptManager.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "../Hardware/OHCIEventCodes.hpp"
#include "../Hardware/RegisterMap.hpp"
#include "../Bus/IRM/IRMClient.hpp"
#include "../Protocols/AVC/AVCDiscovery.hpp"
#include "../Protocols/AVC/CMP/CMPClient.hpp"
#include "../Protocols/Audio/DeviceProtocolFactory.hpp"
#include "../Scheduling/Scheduler.hpp"
#include "../Version/DriverVersion.hpp"
#include "ControllerStateMachine.hpp"
#include "Logging.hpp"

namespace ASFW::Driver {

namespace {
const char* DeviceKindString(Discovery::DeviceKind kind) {
    using Discovery::DeviceKind;
    switch (kind) {
    case DeviceKind::AV_C:
        return "AV/C";
    case DeviceKind::TA_61883:
        return "TA 61883 (AMDTP)";
    case DeviceKind::VendorSpecificAudio:
        return "Vendor Audio";
    case DeviceKind::Storage:
        return "Storage";
    case DeviceKind::Camera:
        return "Camera";
    default:
        return "Unknown";
    }
}

const char* RemoteCmstrResultString(Async::AsyncStatus status) {
    switch (status) {
    case Async::AsyncStatus::kSuccess:
        return "complete";
    case Async::AsyncStatus::kTimeout:
    case Async::AsyncStatus::kBusyRetryExhausted:
        return "timeout";
    case Async::AsyncStatus::kAborted:
    case Async::AsyncStatus::kStaleGeneration:
        return "generation-abort";
    case Async::AsyncStatus::kShortRead:
    case Async::AsyncStatus::kHardwareError:
    case Async::AsyncStatus::kLockCompareFail:
        return "failure";
    }
    return "unknown";
}

const char* RemoteCmstrDetailString(Async::AsyncStatus status) {
    switch (status) {
    case Async::AsyncStatus::kSuccess:
        return "rcode=complete";
    case Async::AsyncStatus::kTimeout:
    case Async::AsyncStatus::kBusyRetryExhausted:
        return "no-response";
    case Async::AsyncStatus::kAborted:
    case Async::AsyncStatus::kStaleGeneration:
        return "generation-stale";
    case Async::AsyncStatus::kHardwareError:
        return "see-async-rcode";
    case Async::AsyncStatus::kShortRead:
        return "short-response";
    case Async::AsyncStatus::kLockCompareFail:
        return "lock-compare-failed";
    }
    return "unknown";
}
} // anonymous namespace

bool ControllerCore::StartDiscoveryScan(const Discovery::ROMScanRequest& request) {
    if (!deps_.romScanner) {
        ASFW_LOG(Discovery, "StartDiscoveryScan: no ROMScanner available");
        return false;
    }

    return deps_.romScanner->Start(
        request,
        [this](Discovery::Generation gen, const std::vector<Discovery::ConfigROM>& roms,
               bool hadBusyNodes) { this->OnDiscoveryScanComplete(gen, roms, hadBusyNodes); });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ControllerCore::OnTopologyReady(const TopologySnapshot& snap) {
    // 1. Advance generation and notify role authority first
    currentGeneration_ = snap.generation;
    roleCoordinator_.OnTopologyChanged(snap.generation, snap);

    // 2. Initialize/update Bus Manager/IRM runtime state (FW-14)
    bmState_.generation = snap.generation;
    bmState_.busBase16 = snap.busBase16;
    bmState_.topologyValid = (snap.graphStatus == Driver::TopologyGraphStatus::Valid);

    if (snap.localNodeId != Driver::kInvalidPhysicalId) {
        bmState_.localNodeId = snap.localNodeId;
    } else {
        bmState_.localNodeId = 0x3F;
    }
    if (snap.irmNodeId != Driver::kInvalidPhysicalId) {
        bmState_.irmNodeId = snap.irmNodeId;
        bmState_.localIsIRM = (snap.localNodeId == snap.irmNodeId);
    } else {
        bmState_.irmNodeId = 0x3F;
        bmState_.localIsIRM = false;
    }

    if (snap.rootNodeId != Driver::kInvalidPhysicalId) {
        bmState_.rootNodeId = snap.rootNodeId;
        bmState_.localIsRoot = (snap.localNodeId == snap.rootNodeId);
    } else {
        bmState_.rootNodeId = 0x3F;
        bmState_.localIsRoot = false;
    }

    // Clear stale BM ownership from previous generation.
    // Only OnLocalWonBM / OnRemoteBM may set these after the new election.
    bmState_.localIsBM = false;
    bmState_.bmNodeId = 0x3F;
    bmState_.bmOwnerSource = ASFW::Bus::BMOwnerSource::Unknown;
    bmState_.ResetGenerationScopedPolicy();

    const bool roleAllowsIRMHost =
        rolePolicy_.roleMode == ASFW::FW::RoleMode::IRMResourceHost ||
        rolePolicy_.roleMode == ASFW::FW::RoleMode::FullBusManager;

    if (localIrmController_) {
        localIrmController_->OnTopologyReady(snap.generation,
                                             bmState_.localNodeId,
                                             bmState_.irmNodeId,
                                             roleAllowsIRMHost);
    }

    if (irmFallback_) {
        irmFallback_->OnTopologyReady(snap, rolePolicy_, GetBusManagerRuntimeState(),
                                      BusResetCoordinator::MonotonicNow());
    }

    EvaluateCyclePolicy();

    if (deps_.topologyMapService) {
        deps_.topologyMapService->Rebuild(snap);
    }

    const bool electionAllowed =
        rolePolicy_.roleMode == ASFW::FW::RoleMode::FullBusManager &&
        rolePolicy_.fullBMActivityLevel >= ASFW::FW::FullBMActivityLevel::ElectionOnly;

    if (deps_.busManagerElectionDriver && electionAllowed) {
        deps_.busManagerElectionDriver->OnTopologyReady(snap, BusResetCoordinator::MonotonicNow());
    }

    EvaluateBusManagerPolicy();

    if (!deps_.romScanner) {
        ASFW_LOG(Discovery, "OnTopologyReady: no ROMScanner available");
        return;
    }

    const uint8_t localNodeId = snap.localNodeId;
    if (localNodeId == Driver::kInvalidPhysicalId) {
        ASFW_LOG(Discovery, "OnTopologyReady: invalid local node ID");
        return;
    }
    BeginRootCapabilityEvidence(snap, localNodeId);

    // Count how many nodes will actually be scanned (exclude local + link-inactive)
    uint32_t scannableCount = 0;
    for (const auto& node : snap.physical.nodes) {
        if (node.physicalId != localNodeId && node.linkActive) {
            scannableCount++;
        }
    }

    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");
    ASFW_LOG(Discovery, "Topology ready gen=%u: %u total nodes, %u scannable (local=%u)",
             snap.generation, snap.nodeCount, scannableCount, localNodeId);
    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");

    Discovery::ROMScanRequest request{};
    request.gen = Discovery::Generation{snap.generation};
    request.topology = snap;
    request.localNodeId = localNodeId;
    request.rootCapabilityCallback = [this](Role::RootCapabilityEvidence evidence) {
        this->OnRootCapabilityProbe(evidence);
    };

    const bool started = StartDiscoveryScan(request);
    if (!started) {
        ASFW_LOG(Discovery, "OnTopologyReady: ROMScanner already busy for gen=%u", snap.generation);
    }

    if (deps_.irmClient) {
        const uint8_t irmNodeId = snap.irmNodeId;
        deps_.irmClient->SetIRMNode(irmNodeId,
                                    Discovery::Generation{snap.generation},
                                    snap.capturedAt);
        ASFW_LOG(Discovery, "IRMClient updated: IRM node=%u, generation=%u", irmNodeId,
                 snap.generation);
    }

    if (deps_.cmpClient) {
        // CMP (PCR) operations target a *specific device's* plug registers, not the IRM node.
        // Device-scoped CMP wiring is done at stream start time (IsochService).
    }
}

void ControllerCore::BeginRootCapabilityEvidence(const TopologySnapshot& snap,
                                                 uint8_t localNodeId) {
    haveRootEvidence_ = false;
    cycleLostWindowActive_ = false;
    ++cycleLostWindowEpoch_;

    (void)cycleObserver_.OnInterrupt(snap.generation, 0);

    if (snap.rootNodeId == Driver::kInvalidPhysicalId) {
        return;
    }

    currentRootEvidence_ = Role::RootCapabilityEvidence{};
    currentRootEvidence_.generation = snap.generation;
    currentRootEvidence_.rootNodeId = snap.rootNodeId;
    currentRootEvidence_.bibReadStatus = Role::RootBibReadStatus::NotStarted;
    currentRootEvidence_.verdict = Role::RootCapability::Unknown;
    haveRootEvidence_ = true;

    if (deps_.configRomStager) {
        const auto decoded =
            ASFW::FW::DecodeBusOptions(deps_.configRomStager->ExpectedBusOptions());
        roleCoordinator_.OnLocalCycleMasterCapability(snap.generation, decoded.cmc);
    }

    if (snap.rootNodeId == localNodeId) {
        if (deps_.configRomStager) {
            const auto decoded =
                ASFW::FW::DecodeBusOptions(deps_.configRomStager->ExpectedBusOptions());
            currentRootEvidence_.bibReadStatus = Role::RootBibReadStatus::Success;
            currentRootEvidence_.cmcKnown = true;
            currentRootEvidence_.cmc = decoded.cmc;
            currentRootEvidence_.configRomHeaderValid = true;
        }
        PublishRootCapabilityEvidence();
        return;
    }

    PublishRootCapabilityEvidence();
    StartRootCycleLostWindow(snap.generation);
}

void ControllerCore::OnRootCapabilityProbe(Role::RootCapabilityEvidence evidence) {
    if (!haveRootEvidence_ || evidence.generation != currentGeneration_ ||
        evidence.generation != currentRootEvidence_.generation ||
        evidence.rootNodeId != currentRootEvidence_.rootNodeId) {
        return;
    }

    currentRootEvidence_.bibReadStatus = evidence.bibReadStatus;
    currentRootEvidence_.cmcKnown = evidence.cmcKnown;
    currentRootEvidence_.cmc = evidence.cmc;
    currentRootEvidence_.configRomHeaderValid = evidence.configRomHeaderValid;
    if (currentRootEvidence_.bibReadStatus == Role::RootBibReadStatus::Success &&
        cycleLostWindowActive_) {
        cycleLostWindowActive_ = false;
        ++cycleLostWindowEpoch_;
        if (deps_.hardware && deps_.interrupts) {
            deps_.interrupts->MaskInterrupts(deps_.hardware.get(), IntEventBits::kCycleLost);
        }
    }
    PublishRootCapabilityEvidence();
    EvaluateCyclePolicy();
}

void ControllerCore::StartRootCycleLostWindow(uint32_t generation) {
    if (!haveRootEvidence_ || generation != currentRootEvidence_.generation ||
        !deps_.hardware || !deps_.interrupts || !deps_.scheduler) {
        return;
    }

    cycleLostWindowActive_ = true;
    const uint32_t epoch = ++cycleLostWindowEpoch_;

    deps_.hardware->ClearIntEvents(IntEventBits::kCycleLost);
    deps_.interrupts->UnmaskInterrupts(deps_.hardware.get(), IntEventBits::kCycleLost);

    constexpr uint64_t kCycleLostObservationWindowNs = 2ULL * 1'000'000ULL;
    deps_.scheduler->DispatchAsyncAfter(kCycleLostObservationWindowNs, [this, generation, epoch] {
        this->CompleteRootCycleLostWindow(generation, epoch, false);
    });
}

void ControllerCore::CompleteRootCycleLostWindow(uint32_t generation, uint32_t epoch,
                                                 bool cycleLost) {
    if (!cycleLostWindowActive_ || epoch != cycleLostWindowEpoch_ ||
        !haveRootEvidence_ || generation != currentRootEvidence_.generation) {
        return;
    }

    cycleLostWindowActive_ = false;
    if (deps_.hardware && deps_.interrupts) {
        deps_.interrupts->MaskInterrupts(deps_.hardware.get(), IntEventBits::kCycleLost);
    }

    if (!cycleLost) {
        (void)cycleObserver_.MarkCycleContinuityObserved(generation);
    }

    currentRootEvidence_.cycleObservationComplete = true;
    currentRootEvidence_.cycles = cycleObserver_.Observation();
    PublishRootCapabilityEvidence();
}

void ControllerCore::PublishRootCapabilityEvidence() {
    if (!haveRootEvidence_) {
        return;
    }

    currentRootEvidence_.verdict = Role::DeriveRootCapabilityVerdict(
        currentRootEvidence_.bibReadStatus,
        currentRootEvidence_.cmcKnown,
        currentRootEvidence_.cmc,
        currentRootEvidence_.cycleObservationComplete,
        currentRootEvidence_.cycles);
    roleCoordinator_.OnRootCapabilityEvidence(currentRootEvidence_.generation,
                                              currentRootEvidence_);
    SyncBusManagerRuntimeState();
    EvaluateBusManagerPolicy();
    EvaluateCyclePolicy();
}

void ControllerCore::ForceRootAndReset(uint8_t targetRoot, Role::RoleResetFlavor flavor,
                                       uint8_t gapCount, uint32_t generation) {
    if (generation != currentGeneration_ || !deps_.busReset) {
        return;
    }

    const bool longReset = flavor == Role::RoleResetFlavor::Long;
    const std::optional<uint8_t> gap =
        (gapCount != 0U) ? std::optional<uint8_t>{gapCount} : std::nullopt;

    std::optional<bool> setContender = std::nullopt;
    if (const auto topo = LatestTopology(); topo && topo->localNodeId == targetRoot) {
        setContender = true;
    }

    ASFW_LOG(Controller,
             "RoleCoordinator: force root target=%u gen=%u reset=%{public}s gap=%u contender=%d",
             targetRoot, generation, longReset ? "Long" : "Short", gap.value_or(0),
             setContender.value_or(false) ? 1 : 0);
    deps_.busReset->RequestRolePolicyReset(targetRoot, longReset, gap, setContender,
                                           "RoleCoordinator force-root");
}

void ControllerCore::EnableRemoteCycleMaster(uint8_t rootNodeId, uint32_t generation) {
    if (generation != currentGeneration_ || !busImpl_) {
        return;
    }

    const Async::FWAddress address{Async::FWAddress::QualifiedAddressParts{
        .addressHi = ASFW::FW::kCSRRegSpaceHi,
        .addressLo = ASFW::FW::kCSRRemoteStateSet,
        .nodeID = static_cast<uint16_t>(0xFFC0u | (rootNodeId & 0x3Fu)),
    }};

    ASFW_LOG(Controller,
             "RoleCoordinator: remote CMSTR write submitted gen=%u root=%u addr=0x%04x:%08x payload=0x%08x",
             generation, rootNodeId, address.addressHi, address.addressLo,
             ASFW::FW::kCSRStateBitCMSTR);

    busImpl_->WriteQuad(ASFW::FW::Generation{generation},
                        ASFW::FW::NodeId{rootNodeId},
                        address,
                        ASFW::FW::kCSRStateBitCMSTR,
                        ASFW::FW::FwSpeed::S100,
                        [generation, rootNodeId](Async::AsyncStatus status,
                                                 std::span<const uint8_t>) {
        ASFW_LOG(Controller,
                 "RoleCoordinator: remote CMSTR write result gen=%u root=%u result=%{public}s status=%{public}s detail=%{public}s",
                 generation, rootNodeId, RemoteCmstrResultString(status),
                 Async::ToString(status), RemoteCmstrDetailString(status));
    });
}

void ControllerCore::EnableLocalCycleMaster(uint32_t generation) {
    if (generation != currentGeneration_ || !deps_.hardware) {
        return;
    }

    ASFW_LOG(Controller, "RoleCoordinator: enabling local cycleMaster gen=%u", generation);
    deps_.hardware->SetLinkControlBits(LinkControlBits::kCycleMaster);
}

void ControllerCore::ClearLocalContenderAndDelegate(uint8_t targetRoot, uint32_t generation) {
    if (generation != currentGeneration_ || !deps_.busReset) {
        return;
    }

    ASFW_LOG(Controller,
             "RoleCoordinator: clear local contender and delegate root target=%u gen=%u",
             targetRoot, generation);
    deps_.busReset->RequestRolePolicyReset(targetRoot,
                                           /*longReset=*/false,
                                           std::nullopt,
                                           false,
                                           "RoleCoordinator delegate-root");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ControllerCore::OnDiscoveryScanComplete(Discovery::Generation gen,
                                             const std::vector<Discovery::ConfigROM>& roms,
                                             bool hadBusyNodes) const {
    if (!deps_.romStore || !deps_.deviceRegistry || !deps_.speedPolicy) {
        ASFW_LOG(Discovery, "OnDiscoveryScanComplete: missing Discovery dependencies");
        return;
    }

    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");
    ASFW_LOG(Discovery, "ROM scan complete for gen=%u, processing results...", gen.value);

    ASFW_LOG(Discovery, "Discovered %zu ROMs (hadBusy=%d)", roms.size(), hadBusyNodes);

    // Propagate busy-node flag to BusResetCoordinator so it can delay
    // the next discovery if DICE-class devices are still booting.
    //
    // CRITICAL: Only clear the delay when a scan actually produced results.
    // If the scan found 0 remote nodes (e.g. Saffire link inactive during boot),
    // we learned nothing about whether the device recovered — keep the delay
    // so the next bus reset doesn't immediately start a doomed scan.
    if (deps_.busReset) {
        if (hadBusyNodes) {
            deps_.busReset->SetPreviousScanHadBusyNodes(true);
            ASFW_LOG(Discovery, "ROM scan had busy nodes — next discovery will be delayed");
        } else if (!roms.empty()) {
            // Successful scan with actual results — device is responding, clear delay
            deps_.busReset->SetPreviousScanHadBusyNodes(false);
            ASFW_LOG(Discovery, "ROM scan succeeded with %zu ROMs — clearing discovery delay",
                     roms.size());
        } else {
            ASFW_LOG(Discovery, "ROM scan produced 0 ROMs — keeping previous delay state");
            // Intentionally do NOT clear previousScanHadBusyNodes.
            // Escalate the delay: we learned nothing, device may still be booting.
            deps_.busReset->EscalateDiscoveryDelay();
        }
    }

    bool zeroRomScanInconclusive = false;
    if (deps_.topology) {
        if (const auto latestTopology = deps_.topology->LatestSnapshot()) {
            zeroRomScanInconclusive = Discovery::IsZeroRomScanInconclusive(
                gen, roms.size(), *latestTopology);
        }
    }

    std::unordered_set<uint64_t> discoveredGuids;
    discoveredGuids.reserve(roms.size());
    std::unordered_set<uint64_t> seenGuids;
    std::unordered_set<uint64_t> duplicateGuids;
    seenGuids.reserve(roms.size());
    duplicateGuids.reserve(roms.size());

    for (const auto& rom : roms) {
        if (rom.bib.guid == 0) {
            continue;
        }
        if (!seenGuids.insert(rom.bib.guid).second) {
            duplicateGuids.insert(rom.bib.guid);
        }
    }

    for (const auto& rom : roms) {
        const auto nodeId = ASFW::Discovery::TryOperationalNodeId(rom.nodeId);
        if (!nodeId.has_value()) {
            ASFW_LOG(Discovery, "Skipping ROM with invalid nodeId=%u gen=%u during discovery",
                     rom.nodeId, rom.gen.value);
            continue;
        }

        if (rom.bib.guid == 0) {
            // Cross-validated with Apple: IOFireWireFamily.kmodproj/IOFireWireController.cpp:2992-3037.
            // Minimal/zero-GUID ROMs are not registered as normal device identities.
            ASFW_LOG(Discovery,
                     "Skipping ROM with GUID=0 gen=%u node=%u; minimal/invalid Config ROM cannot "
                     "anchor a stable device",
                     rom.gen.value,
                     rom.nodeId);
            continue;
        }

        if (duplicateGuids.contains(rom.bib.guid)) {
            ASFW_LOG(Discovery,
                     "Skipping duplicate GUID=0x%016llx gen=%u node=%u during discovery",
                     rom.bib.guid,
                     rom.gen.value,
                     rom.nodeId);
            deps_.deviceRegistry->MarkDuplicateGuid(gen, rom.bib.guid, *nodeId);
            continue;
        }

        deps_.romStore->Insert(rom);

        auto policy = deps_.speedPolicy->ForNode(*nodeId);

        auto& bus = this->Bus();
        auto& deviceRecord = deps_.deviceRegistry->UpsertFromROM(
            rom, policy, static_cast<Async::IFireWireBusOps*>(&bus),
            static_cast<Async::IFireWireBusInfo*>(&bus),
            deps_.irmClient.get());
        discoveredGuids.insert(deviceRecord.guid);

        if (deps_.deviceManager) {
            auto fwDevice = deps_.deviceManager->UpsertDevice(deviceRecord, rom);

            if (fwDevice) {
                ASFW_LOG(Discovery, "  Created FWDevice with %zu units",
                         fwDevice->GetUnits().size());
            }
        }

        ASFW_LOG(Discovery, "═══════════════════════════════════════");
        ASFW_LOG(Discovery, "Device Discovered:");
        ASFW_LOG(Discovery, "  GUID: 0x%016llx", deviceRecord.guid);
        ASFW_LOG(Discovery, "  Vendor: 0x%06x", deviceRecord.vendorId);
        ASFW_LOG(Discovery, "  Model: 0x%06x", deviceRecord.modelId);
        ASFW_LOG(Discovery, "  Node: %u (gen=%u)", rom.nodeId, rom.gen.value);
        ASFW_LOG(Discovery, "  Kind: %{public}s", DeviceKindString(deviceRecord.kind));
        ASFW_LOG(Discovery, "  Audio Candidate: %{public}s",
                 deviceRecord.isAudioCandidate ? "YES" : "NO");
    }

    if (deps_.deviceManager && !zeroRomScanInconclusive) {
        auto devices = deps_.deviceManager->GetAllDevices();
        for (const auto& device : devices) {
            if (!device) {
                continue;
            }

            const uint64_t guid = device->GetGUID();
            if (discoveredGuids.contains(guid)) {
                continue;
            }

            ASFW_LOG(Discovery,
                     "Device missing from generation %u scan - marking lost (GUID=0x%016llx)",
                     gen.value, guid);
            deps_.deviceManager->MarkDeviceLost(guid);
        }
    } else if (deps_.deviceManager && zeroRomScanInconclusive) {
        ASFW_LOG(Discovery,
                 "ROM scan for gen=%u produced 0 ROMs but topology still has remote "
                 "link-active nodes; keeping existing devices until a conclusive scan",
                 gen.value);
    }

    ASFW_LOG(Discovery, "Discovery complete: %zu devices processed in gen=%u", roms.size(),
             gen.value);
    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");
}

void ControllerCore::OnLocalWonBM(uint32_t generation, uint8_t localNodeId) {
    bmState_.generation = generation;
    bmState_.localIsBM = true;
    bmState_.bmNodeId = localNodeId;
    bmState_.bmOwnerSource = ASFW::Bus::BMOwnerSource::LocalWonElection;
    if (deps_.busManagerElectionDriver) {
        bmState_.lastBusManagerIdOldValue = deps_.busManagerElectionDriver->FSM().LastOldValue();
        bmState_.staleElectionAbortCount = deps_.busManagerElectionDriver->FSM().StaleElectionAbortCount();
    }
    EvaluateBusManagerPolicy();
    EvaluateCyclePolicy();
}

void ControllerCore::OnRemoteBM(uint32_t generation, uint8_t remoteNodeId) {
    bmState_.generation = generation;
    bmState_.localIsBM = false;
    bmState_.bmNodeId = remoteNodeId;
    bmState_.bmOwnerSource = ASFW::Bus::BMOwnerSource::RemoteWonElection;
    if (deps_.busManagerElectionDriver) {
        bmState_.lastBusManagerIdOldValue = deps_.busManagerElectionDriver->FSM().LastOldValue();
        bmState_.staleElectionAbortCount = deps_.busManagerElectionDriver->FSM().StaleElectionAbortCount();
    }
    EvaluateBusManagerPolicy();
    EvaluateCyclePolicy();
}

void ControllerCore::OnBMElectionFailed(uint32_t generation, ASFW::Async::AsyncStatus status) {
    bmState_.generation = generation;
    bmState_.failedElectionCount++;
    if (deps_.busManagerElectionDriver) {
        bmState_.staleElectionAbortCount = deps_.busManagerElectionDriver->FSM().StaleElectionAbortCount();
    }
    EvaluateBusManagerPolicy();
    EvaluateCyclePolicy();
}

void ControllerCore::EvaluateBusManagerPolicy() noexcept {
    if (bmPolicyCoordinator_) {
        bmPolicyCoordinator_->Evaluate(GetBusManagerRuntimeState());
    }
}

void ControllerCore::EvaluateCyclePolicy() noexcept {
    if (!cyclePolicy_) {
        return;
    }

    const auto& state = GetBusManagerRuntimeState();
    Bus::CyclePolicyInputs in{};
    in.generation = state.generation;
    in.busBase16 = state.busBase16;
    in.localNodeId = state.localNodeId;
    in.rootNodeId = state.rootNodeId;
    in.irmNodeId = state.irmNodeId;
    in.bmNodeId = state.bmNodeId;
    in.topologyValid = state.topologyValid;
    in.localIsRoot = state.localIsRoot;
    in.localIsIRM = state.localIsIRM;
    in.localIsBM = state.localIsBM;

    if (irmFallback_) {
        const auto& snap = irmFallback_->Snapshot();
        in.irmFallbackNoBMDetected = snap.noBusManagerDetected;
        in.irmFallbackGateOpen = snap.annexHGateOpen;
    }

    in.cycleStartObserved = state.cycleStartObserved;
    in.cycleStartSourceNode = state.cycleStartSourceNode;
    in.rootCmcKnown = state.rootCmcKnown;
    in.rootCmcCapable = state.rootCmcCapable;
    in.roleMode = rolePolicy_.roleMode;
    in.activityLevel = rolePolicy_.fullBMActivityLevel;

    cyclePolicy_->Evaluate(in, *this);

    if (cyclePolicy_->Snapshot().lastDecision == Bus::CyclePolicyDecision::RootSelectionRequired) {
        EvaluateRootSelectionPolicy();
    }
}

void ControllerCore::EvaluateRootSelectionPolicy() noexcept {
    if (!rootSelection_) {
        return;
    }

    const auto& state = GetBusManagerRuntimeState();

    Bus::RootSelectionInputs in{};
    in.generation = state.generation;
    in.busBase16 = state.busBase16;
    in.roleMode = rolePolicy_.roleMode;
    in.activityLevel = rolePolicy_.fullBMActivityLevel;
    in.topologyValid = state.topologyValid;
    in.localNodeId = state.localNodeId;
    in.rootNodeId = state.rootNodeId;
    in.irmNodeId = state.irmNodeId;
    in.bmNodeId = state.bmNodeId;
    in.localIsRoot = state.localIsRoot;
    in.localIsIRM = state.localIsIRM;
    in.localIsBM = state.localIsBM;
    in.cycleStartObserved = state.cycleStartObserved;
    in.rootCmcKnown = state.rootCmcKnown;
    in.rootCmcCapable = state.rootCmcCapable;

    // Populate local CMC evidence from BIB
    const auto advertisedCaps = ASFW::Driver::DeriveBusOptionsCapabilities(rolePolicy_);
    in.localCmcKnown = true;
    in.localCmcCapable = advertisedCaps.cmc;

    in.currentGapCount = LatestTopology().has_value() ? LatestTopology()->gapCount : static_cast<uint8_t>(63);

    if (irmFallback_) {
        const auto& fallback = irmFallback_->Snapshot();
        in.irmFallbackGateOpen = fallback.annexHGateOpen;
        in.irmFallbackNoBMDetected = fallback.noBusManagerDetected;
    }

    const auto topo = LatestTopology();
    if (topo.has_value()) {
        in.topology = &(*topo);
    }

    rootSelection_->Evaluate(in, *this);
}

bool ControllerCore::ForceRootAndResetForBMPolicy(uint32_t generation,
                                                  uint8_t targetRoot,
                                                  bool longReset,
                                                  std::optional<uint8_t> gapCount) {
    if (generation != currentGeneration_ || !deps_.busReset) {
        return false;
    }

    std::optional<bool> setContender = std::nullopt;

    if (const auto topo = LatestTopology(); topo && topo->localNodeId == targetRoot) {
        setContender = true;
    }

    deps_.busReset->RequestRolePolicyReset(targetRoot,
                                           longReset,
                                           gapCount,
                                           setContender,
                                           "BM root-selection policy");
    return true;
}

bool ControllerCore::EnableLocalCycleMasterMutation(uint32_t generation) {
    if (generation != currentGeneration_ || !deps_.hardware) {
        return false;
    }
    return deps_.hardware->SetLocalCycleMasterEnabled(true);
}

Async::AsyncHandle ControllerCore::WriteRemoteStateSetCmstr(uint32_t generation,
                                                           uint16_t busBase16,
                                                           uint8_t targetNodeId) {
    if (generation != currentGeneration_ || !busImpl_) {
        return {};
    }

    const Async::FWAddress address{Async::FWAddress::QualifiedAddressParts{
        .addressHi = ASFW::FW::kCSRRegSpaceHi,
        .addressLo = ASFW::FW::kCSRRemoteStateSet,
        .nodeID = static_cast<uint16_t>(busBase16 | (targetNodeId & 0x3Fu)),
    }};

    std::weak_ptr<ControllerCore> weakThis = shared_from_this();
    return busImpl_->WriteQuad(
        FW::Generation{generation}, FW::NodeId{targetNodeId}, address,
        ASFW::FW::kCSRStateBitCMSTR, FW::FwSpeed::S100,
        [weakThis, generation, targetNodeId](Async::AsyncStatus status, std::span<const uint8_t>) {
            auto self = weakThis.lock();
            if (self) {
                self->OnRemoteCmstrComplete(generation, targetNodeId, status);
            }
        });
}

void ControllerCore::OnRemoteCmstrComplete(uint32_t generation, uint8_t targetNode,
                                           Async::AsyncStatus status) noexcept {
    if (cyclePolicy_) {
        cyclePolicy_->OnRemoteCmstrComplete(generation, targetNode, status);
    }
    HandleRemoteCmstrCallback(generation, targetNode, status);
}

void ControllerCore::SendRemoteCmstr(uint8_t rootNodeId, uint32_t generation) {
    if (generation != currentGeneration_ || !busImpl_) {
        return;
    }

    const Async::FWAddress address{Async::FWAddress::QualifiedAddressParts{
        .addressHi = ASFW::FW::kCSRRegSpaceHi,
        .addressLo = ASFW::FW::kCSRRemoteStateSet,
        .nodeID = static_cast<uint16_t>(0xFFC0u | (rootNodeId & 0x3Fu)),
    }};

    bmState_.lastRemoteCmstrGeneration = generation;
    bmState_.lastRemoteCmstrTargetNode = rootNodeId;

    std::weak_ptr<ControllerCore> weakSelf = std::static_pointer_cast<ControllerCore>(shared_from_this());

    busImpl_->WriteQuad(ASFW::FW::Generation{generation},
                        ASFW::FW::NodeId{rootNodeId},
                        address,
                        ASFW::FW::kCSRStateBitCMSTR,
                        ASFW::FW::FwSpeed::S100,
                        [weakSelf, generation, rootNodeId](Async::AsyncStatus status,
                                                           std::span<const uint8_t>) {
        auto self = weakSelf.lock();
        if (!self) {
            return;
        }
        ASFW_LOG(Controller,
                 "ControllerCore: remote CMSTR write callback gen=%u root=%u result=%{public}s",
                 generation, rootNodeId, Async::ToString(status));
        self->HandleRemoteCmstrCallback(generation, rootNodeId, status);
    });
}

void ControllerCore::HandleRemoteCmstrCallback(uint32_t generation, uint8_t rootNodeId, ASFW::Async::AsyncStatus status) {
    if (generation == currentGeneration_) {
        bmState_.lastRemoteCmstrResult = static_cast<uint32_t>(status);
        EvaluateBusManagerPolicy();
        EvaluateCyclePolicy();
    }
}

void ControllerCore::SyncBusManagerRuntimeState() const noexcept {
    if (deps_.busManagerElectionDriver) {
        bmState_.staleElectionAbortCount = deps_.busManagerElectionDriver->FSM().StaleElectionAbortCount();
        bmState_.lastBusManagerIdOldValue = deps_.busManagerElectionDriver->FSM().LastOldValue();
    }
    if (deps_.csrResponder) {
        bmState_.unexpectedResourceCsrSoftwareCount = deps_.csrResponder->UnexpectedResourceCsrSoftwareCount();
    }

    // Populate root evidence from role coordinator
    const auto rootEvidence = roleCoordinator_.LastRootEvidence();
    if (rootEvidence.generation == bmState_.generation) {
        bmState_.rootCmcKnown = rootEvidence.cmcKnown;
        bmState_.rootCmcCapable = rootEvidence.cmc;

        const auto cycleObs = cycleObserver_.Observation();
        bmState_.cycleStartObserved = cycleObs.cycleStartObserved;
        bmState_.cycleStartSourceNode = rootEvidence.rootNodeId; // For now, assume root is source
    } else {
        bmState_.rootCmcKnown = false;
        bmState_.rootCmcCapable = false;
        bmState_.cycleStartObserved = false;
        bmState_.cycleStartSourceNode = 0x3F;
    }

    // Populate submode level
    bmState_.fullBMActivityLevel = static_cast<uint8_t>(rolePolicy_.fullBMActivityLevel);
}

} // namespace ASFW::Driver
