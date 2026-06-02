#include "DiagnosticsService.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../Bus/BusManager.hpp"
#include "../Bus/BusResetCoordinator.hpp"
#include "../Bus/BusManager/BusManagerElectionDriver.hpp"
#include "../Bus/BusManager/CyclePolicyCoordinator.hpp"
#include "../Bus/BusManager/RootSelectionCoordinator.hpp"
#include "../Bus/BusManager/GapPolicyCoordinator.hpp"
#include "../Bus/IRM/IRMFallbackCoordinator.hpp"
#include "../Common/CSRSpace.hpp"
#include "../Controller/ControllerConfig.hpp"
#include "../Async/AsyncSubsystem.hpp"
#include "../Async/Interfaces/IAsyncSubsystemPort.hpp"
#include "../Debug/AsyncTraceCapture.hpp"
#include "../Bus/CSR/BroadcastChannelCSR.hpp"
#include "../Bus/CSR/TopologyMapService.hpp"
#include "../Bus/CSR/SpeedMapService.hpp"
#include "../Bus/CSR/CSRContractVerifier.hpp"
#include <cstring>
#include <DriverKit/IOLib.h>

namespace ASFW::Diagnostics {

namespace {

// The ABI `ports` field carries an ASFWDiagPortState enum; the topology stores the
// driver's internal PortState. The two enums are defined to share wire-format values,
// so this is a checked straight-through map (never the bare cast that previously let
// the impoverished ABI enum mis-render Child as "Unknown" and NotConnected as "Child").
static_assert(static_cast<uint32_t>(Driver::PortState::NotPresent) == ASFWDiagPortStateNotPresent);
static_assert(static_cast<uint32_t>(Driver::PortState::NotActive) == ASFWDiagPortStateNotConnected);
static_assert(static_cast<uint32_t>(Driver::PortState::Parent) == ASFWDiagPortStateParent);
static_assert(static_cast<uint32_t>(Driver::PortState::Child) == ASFWDiagPortStateChild);

uint32_t PortStateToDiag(Driver::PortState state) noexcept {
    switch (state) {
        case Driver::PortState::NotPresent: return ASFWDiagPortStateNotPresent;
        case Driver::PortState::NotActive:  return ASFWDiagPortStateNotConnected;
        case Driver::PortState::Parent:     return ASFWDiagPortStateParent;
        case Driver::PortState::Child:      return ASFWDiagPortStateChild;
    }
    return ASFWDiagPortStateNotPresent;
}

// The ABI `speed` field carries an ASFWDiagSpeed enum, but the topology stores raw Mbps.
uint32_t MbpsToDiagSpeed(uint32_t mbps) noexcept {
    switch (mbps) {
        case 100:  return ASFWDiagSpeedS100;
        case 200:  return ASFWDiagSpeedS200;
        case 400:  return ASFWDiagSpeedS400;
        case 800:  return ASFWDiagSpeedS800;
        case 1600: return ASFWDiagSpeedS1600;
        case 3200: return ASFWDiagSpeedS3200;
        default:   return ASFWDiagSpeedUnknown;
    }
}

uint64_t MonotonicNowNs() noexcept {
    static mach_timebase_info_data_t timebase{};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    return (mach_absolute_time() * timebase.numer) / timebase.denom;
}

void InitHeader(ASFWDiagHeader* header, uint32_t structSize, uint32_t generation, uint32_t seq) noexcept {
    header->abiVersion = ASFW_DIAG_ABI_VERSION;
    header->structSize = structSize;
    header->status = ASFWDiagStatusOK;
    header->generation = generation;
    header->snapshotSeq = seq;
    
    static mach_timebase_info_data_t timebase{};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    header->timestampNs = (mach_absolute_time() * timebase.numer) / timebase.denom;
}

} // namespace

DiagnosticsService::DiagnosticsService(Driver::ControllerCore* controller) noexcept
    : controller_(controller) {}

ASFWDiagStatus DiagnosticsService::CollectBusContract(ASFWDiagBusContract* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagBusContract));
    
    auto topoOpt = controller_->LatestTopology();
    if (topoOpt) {
        const auto& topo = *topoOpt;
        out->busId = topo.busNumber.value_or(0);
        out->localNode = topo.localNodeId;
        out->rootNode = topo.rootNodeId;
        out->irmNode = topo.irmNodeId;
        // BM is NOT the IRM. The IRM is elected passively from Self-ID; the BM is
        // a separate role won by a lock(compare_swap) on the IRM's BUS_MANAGER_ID
        // register, and a bus may have no BM at all. Report the real tracked BM
        // identity (BusManagerRuntimeState.bmNodeId; 0x3F == none/unknown, the
        // BUS_MANAGER_ID reset value) instead of echoing the IRM. In ClientOnly
        // we never contend, so this honestly reads "none" with BM Owner Source
        // = Unknown. Determining a remote BM (probing 0x21C) is deferred Tier 2.
        out->bmNode = controller_->GetBusManagerRuntimeState().bmNodeId;
        out->nodeCount = topo.nodeCount;
        out->gapCount = topo.gapCount;
        out->maxHops = topo.physical.busDiameterHops;
    } else {
        out->localNode = 0xFF;
        out->rootNode = 0xFF;
        out->irmNode = 0xFF;
        out->bmNode = 0xFF;
    }

    const auto& roleCoordinator = controller_->GetRoleCoordinator();
    out->cycleStartObserved = roleCoordinator.LastRootEvidence().cycles.cycleStartObserved ? 1 : 0;
    out->cycleStartSourceNode = roleCoordinator.LastRootEvidence().rootNodeId;
    out->roleVerdict = static_cast<uint32_t>(roleCoordinator.LastAction().kind);
    out->rolePolicyMode = static_cast<uint32_t>(roleCoordinator.LastAction().reset);

    // Read hardware LinkControl to check cycle master/timer enable status
    auto* hw = controller_->GetHardware();
    if (hw) {
        const uint32_t linkCtrl = hw->ReadLinkControl();
        out->localCycleMasterEnabled = (linkCtrl & Driver::LinkControlBits::kCycleMaster) ? 1 : 0;
        out->localCycleTimerEnabled = (linkCtrl & Driver::LinkControlBits::kCycleTimerEnable) ? 1 : 0;
    }

    auto* busReset = controller_->GetBusResetCoordinator();
    if (busReset) {
        out->asfwInitiatedResetCount = busReset->Diagnostics().softwareResetIssuedCount;
    }

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagBusContract), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectTopology(ASFWDiagTopology* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagTopology));

    auto topoOpt = controller_->LatestTopology();
    if (!topoOpt) {
        out->valid = 0;
    } else {
        const auto& topo = *topoOpt;
        out->valid = 1;
        out->localNode = topo.localNodeId;
        out->rootNode = topo.rootNodeId;
        out->irmNode = topo.irmNodeId;
        out->nodeCount = topo.nodeCount;
        
        // Copy raw Self-IDs
        const auto& rawQuads = topo.rawSelfIdQuadlets;
        out->rawSelfIdCount = static_cast<uint32_t>(rawQuads.size());
        const uint32_t copyCount = (out->rawSelfIdCount > ASFW_DIAG_MAX_SELF_ID_QUADS) 
                                       ? ASFW_DIAG_MAX_SELF_ID_QUADS 
                                       : out->rawSelfIdCount;
        for (uint32_t i = 0; i < copyCount; ++i) {
            out->rawSelfIds[i] = rawQuads[i];
        }
        out->selfIdSequenceCount = topo.selfIdSequenceCount;
        out->enumeratorError = (topo.selfIdStatus == Driver::SelfIDStreamStatus::Valid) ? 0 : 1;
        out->gapCount = topo.gapCount;
        out->busBase16 = topo.busBase16;

        // Copy decoded nodes
        const uint32_t copyNodes = (topo.physical.nodes.size() > ASFW_DIAG_MAX_NODES) 
                                       ? ASFW_DIAG_MAX_NODES 
                                       : static_cast<uint32_t>(topo.physical.nodes.size());
        for (uint32_t i = 0; i < copyNodes; ++i) {
            const auto& srcNode = topo.physical.nodes[i];
            auto& dstNode = out->nodes[i];
            dstNode.nodeId = srcNode.physicalId;
            dstNode.linkActive = srcNode.linkActive ? 1 : 0;
            dstNode.contender = srcNode.contender ? 1 : 0;
            dstNode.speed = MbpsToDiagSpeed(srcNode.maxSpeedMbps);
            dstNode.powerClass = srcNode.powerClass;
            dstNode.gapCount = srcNode.gapCount;
            dstNode.portCount = srcNode.portCount;
            dstNode.isLocal = (topo.localNodeId != Driver::kInvalidPhysicalId && srcNode.physicalId == topo.localNodeId) ? 1 : 0;
            dstNode.isRoot = (topo.rootNodeId != Driver::kInvalidPhysicalId && srcNode.physicalId == topo.rootNodeId) ? 1 : 0;
            dstNode.isIRM = (topo.irmNodeId != Driver::kInvalidPhysicalId && srcNode.physicalId == topo.irmNodeId) ? 1 : 0;
            dstNode.initiatedReset = srcNode.initiatedReset ? 1 : 0;
            
            const uint32_t copyPorts = (srcNode.reportedPorts.size() > ASFW_DIAG_MAX_PORTS)
                                           ? ASFW_DIAG_MAX_PORTS
                                           : static_cast<uint32_t>(srcNode.reportedPorts.size());
            // TODO (worth considering, not urgent): parentPort is derived from the
            // raw `reportedPorts` while adjacency comes from the reconstructed
            // `links[]`, and the two are never cross-validated here. The normalized
            // graph (SelfIDTopologyNormalizer::NormalizeFromLocal) already resolves
            // a validated, acyclic parent/child orientation — emitting from that
            // instead would make the exported tree self-consistent by construction
            // and remove any chance of the app re-deriving a back-edge. Driver data
            // is currently sound; this is a robustness/clarity improvement only.
            dstNode.parentPort = 0xFFFFFFFFu;
            for (uint32_t p = 0; p < copyPorts; ++p) {
                dstNode.ports[p] = PortStateToDiag(srcNode.reportedPorts[p]);

                // Physical adjacency, parallel to ports[]: pack remote node id
                // and remote port, or 0xFFFFFFFF when the port is not connected.
                const auto& link = srcNode.links[p];
                if (link.connected) {
                    dstNode.links[p] = (static_cast<uint32_t>(link.remoteNodeId) << 8) |
                                       static_cast<uint32_t>(link.remotePort);
                } else {
                    dstNode.links[p] = 0xFFFFFFFFu;
                }

                // First port reported as Parent points toward the root.
                if (dstNode.parentPort == 0xFFFFFFFFu &&
                    srcNode.reportedPorts[p] == Driver::PortState::Parent) {
                    dstNode.parentPort = p;
                }
            }
            // Ports beyond portCount carry no adjacency.
            for (uint32_t p = copyPorts; p < ASFW_DIAG_MAX_PORTS; ++p) {
                dstNode.links[p] = 0xFFFFFFFFu;
            }
        }
    }

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagTopology), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectRoleCoordinator(ASFWDiagRoleCoordinator* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagRoleCoordinator));

    const auto& rc = controller_->GetRoleCoordinator();
    out->policyMode = 0; // Skeleton default
    out->lastDecision = static_cast<uint32_t>(rc.LastAction().kind);
    out->lastAction = static_cast<uint32_t>(rc.LastAction().kind);
    out->lastActionResult = static_cast<uint32_t>(rc.LastAction().reset);
    out->bmRetryCount = rc.ResetRetriesThisTopology();
    
    const auto& evidence = rc.LastRootEvidence();
    out->cycleStartObserved = evidence.cycles.cycleStartObserved ? 1 : 0;
    out->cycleStartSourceNode = evidence.rootNodeId;
    out->localCycleMasterEnabled = evidence.cmc ? 1 : 0;
    out->localCycleMasterAllowed = evidence.cmcKnown ? 1 : 0;

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagRoleCoordinator), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectPostResetTiming(ASFWDiagPostResetTiming* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagPostResetTiming));

    auto* busReset = controller_->GetBusResetCoordinator();
    if (!busReset) {
        return ASFWDiagStatusUnavailable;
    }

    const uint64_t nowNs = MonotonicNowNs();
    const auto snap = busReset->PostResetTiming().Snapshot(nowNs);

    out->selfIdComplete = snap.selfIdComplete ? 1 : 0;
    out->generation = snap.generation;
    out->selfIdCompleteNs = snap.selfIdCompleteNs;
    out->nowNs = snap.nowNs;
    out->ageSinceSelfIdNs = snap.ageSinceSelfIdNs;

    out->incumbentBMGate = static_cast<uint32_t>(snap.incumbentBMGate);
    out->nonIncumbentBMGate = static_cast<uint32_t>(snap.nonIncumbentBMGate);
    out->irmFallbackGate = static_cast<uint32_t>(snap.irmFallbackGate);
    out->newIsoAllocationGate = static_cast<uint32_t>(snap.newIsoAllocationGate);

    out->nonIncumbentBMRemainingNs = snap.nonIncumbentBMRemainingNs;
    out->irmFallbackRemainingNs = snap.irmFallbackRemainingNs;
    out->newIsoAllocationRemainingNs = snap.newIsoAllocationRemainingNs;

    out->staleTimerFirings = snap.staleTimerFirings;
    out->suppressedByGeneration = snap.suppressedByGeneration;
    out->suppressedByRolePolicy = snap.suppressedByRolePolicy;

    // Display-only BM candidate classification (policy stays out of the timing
    // core). Only an active FullBusManager is ever a candidate; everything else
    // — including the ObserveOnly default — is NotCandidate, so the report makes
    // clear that an Open BM gate does not imply the local node will contend.
    out->bmCandidateClass = static_cast<uint32_t>(Bus::Timing::BMCandidateClass::NotCandidate);
    const auto& rolePolicy = controller_->GetRolePolicy();
    if (rolePolicy.roleMode == ASFW::FW::RoleMode::FullBusManager &&
        rolePolicy.fullBMActivityLevel >= ASFW::FW::FullBMActivityLevel::ElectionOnly) {
        const auto& bmState = controller_->GetBusManagerRuntimeState();
        const bool incumbent = (bmState.bmNodeId != 0x3F) && (bmState.localNodeId == bmState.bmNodeId);
        out->bmCandidateClass = static_cast<uint32_t>(incumbent
                                                          ? Bus::Timing::BMCandidateClass::Incumbent
                                                          : Bus::Timing::BMCandidateClass::NonIncumbent);
    }

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagPostResetTiming), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectOHCI(ASFWDiagOHCI* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    auto* hw = controller_->GetHardware();
    if (!hw) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagOHCI));

    out->version = hw->Read(Driver::Register32::kVersion);
    out->guidROM = hw->Read(Driver::Register32::kGUIDROM);
    out->atRetries = hw->Read(Driver::Register32::kATRetries);
    out->csrData = hw->Read(Driver::Register32::kCSRData);
    out->csrCompareData = hw->Read(Driver::Register32::kCSRCompareData);
    out->csrControl = hw->Read(Driver::Register32::kCSRControl);
    out->configROMHeader = hw->Read(Driver::Register32::kConfigROMHeader);
    out->busIdRegister = hw->Read(Driver::Register32::kBusID);
    out->busOptions = hw->Read(Driver::Register32::kBusOptions);
    out->guidHi = hw->Read(Driver::Register32::kGUIDHi);
    out->guidLo = hw->Read(Driver::Register32::kGUIDLo);
    out->configROMMap = hw->Read(Driver::Register32::kConfigROMMap);
    out->postedWriteAddressLo = hw->Read(Driver::Register32::kPostedWriteAddressLo);
    out->postedWriteAddressHi = hw->Read(Driver::Register32::kPostedWriteAddressHi);
    out->vendorId = hw->Read(Driver::Register32::kVendorId);
    out->hcControlSet = hw->ReadHCControl();
    out->hcControlClear = out->hcControlSet; // HCControl read back
    out->selfIdBuffer = hw->Read(Driver::Register32::kSelfIDBuffer);
    out->selfIdCount = hw->Read(Driver::Register32::kSelfIDCount);
    out->intEventSet = hw->ReadIntEvent();
    out->intMaskSet = hw->Read(Driver::Register32::kIntMaskSet);
    out->linkControlSet = hw->ReadLinkControl();
    out->linkControlClear = out->linkControlSet;
    out->nodeId = hw->ReadNodeID();
    out->phyControl = hw->Read(Driver::Register32::kPhyControl);
    out->isochronousCycleTimer = hw->ReadCycleTime();

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagOHCI), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectPHY(ASFWDiagPHY* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    auto* hw = controller_->GetHardware();
    if (!hw) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagPHY));
    out->regCount = ASFW_DIAG_MAX_PHY_REGS;

    // Read PHY registers address 0 to 15 safely (ReadPhyRegister serializes using phyLock_ internally).
    // regValidMask records which reads actually succeeded (rdDone) vs failed/timed-out — so a 0xFF
    // from a dead read is distinguishable from a genuine 0xFF (e.g. isolated PHY).
    out->regValidMask = 0;
    for (uint8_t i = 0; i < ASFW_DIAG_MAX_PHY_REGS; ++i) {
        auto optVal = hw->ReadPhyRegister(i);
        if (optVal) {
            out->regs[i] = *optVal;
            out->regValidMask |= (1u << i);
        } else {
            out->regs[i] = 0xFF; // read failed/timed out — bit left clear in regValidMask
        }
    }

    // Decode fields from PHY registers
    // Reg 1: bits[5:0] = gap_count
    if (out->regs[1] != 0xFF) {
        out->gapCount = out->regs[1] & 0x3F;
    }

    // Reg 4: bit[7] = L (link active), bit[6] = C (contender)
    if (out->regs[4] != 0xFF) {
        out->linkOn = (out->regs[4] & 0x80) ? 1 : 0;
        out->contender = (out->regs[4] & 0x40) ? 1 : 0;
    }

    // Get planned/last config targets from BusManager config
    auto* bm = controller_->GetBusManager();
    if (bm) {
        const auto& config = bm->GetConfig();
        out->lastPhyConfigRootId = config.forcedRootNodeID;
        out->lastPhyConfigGapCount = config.forcedGapCount;
    }

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagPHY), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectCSRContract(ASFWDiagCSRContract* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagCSRContract));

    // Get statistics to populate software-visible CSR access counts. OHCI-owned
    // IRM resource CSRs may be accessed by remote nodes without any software AR
    // packet reaching ASFW, so their zero counters are not evidence of no bus
    // activity; they only describe requests that escaped OHCI autonomy and hit
    // the software request chain.
    auto* stats = controller_->AsyncSubsystem().GetInboundCSRStats();
    
    // Static definition of standard FireWire CSR space entries
    struct LocalCSREntryDef {
        uint64_t address;
        uint32_t offset;
        ASFWDiagCSROwner owner;
        bool implemented;
        const char* name;
    };

    // Offsets/ownership follow IEEE 1212 / 1394 + Apple IOFireWireFamilyCommon.h:
    //   STATE_CLEAR=+0x000, STATE_SET=+0x004 (were swapped); BROADCAST_CHANNEL=+0x234 (was 0x22C).
    // IRM resource registers (BUS_MANAGER_ID/BANDWIDTH/CHANNELS) are serviced by
    // the OHCI CSR engine. Remote access telemetry for them is not generally
    // software-visible. TOPOLOGY_MAP is software-owned; SPEED_MAP is obsolete in
    // IEEE 1394-2008, but ASFW serves a bounded software legacy window for readers.
    static const LocalCSREntryDef CSR_DEFS[] = {
        { 0xFFFFF0000000ULL, 0x000, ASFWDiagCSROwnerASFWSoftware,         true,  "STATE_CLEAR" },
        { 0xFFFFF0000004ULL, 0x004, ASFWDiagCSROwnerASFWSoftware,         true,  "STATE_SET" },
        { 0xFFFFF000021CULL, 0x21C, ASFWDiagCSROwnerOHCIHardware,         true,  "BUS_MANAGER_ID" },
        { 0xFFFFF0000220ULL, 0x220, ASFWDiagCSROwnerOHCIHardware,         true,  "BANDWIDTH_AVAILABLE" },
        { 0xFFFFF0000224ULL, 0x224, ASFWDiagCSROwnerOHCIHardware,         true,  "CHANNELS_AVAILABLE_HI" },
        { 0xFFFFF0000228ULL, 0x228, ASFWDiagCSROwnerOHCIHardware,         true,  "CHANNELS_AVAILABLE_LO" },
        { 0xFFFFF0000234ULL, 0x234, ASFWDiagCSROwnerASFWSoftware,         true,  "BROADCAST_CHANNEL" },
        { 0xFFFFF0000400ULL, 0x400, ASFWDiagCSROwnerOHCIHardware,         true,  "CONFIG_ROM" },
        { 0xFFFFF0001000ULL, 0x1000, ASFWDiagCSROwnerASFWSoftware,         true,  "TOPOLOGY_MAP" },
        { 0xFFFFF0002000ULL, 0x2000, ASFWDiagCSROwnerASFWSoftware,         true,  "SPEED_MAP" }
    };

    constexpr uint32_t entryCount = sizeof(CSR_DEFS) / sizeof(CSR_DEFS[0]);
    out->entryCount = entryCount;

    for (uint32_t i = 0; i < entryCount; ++i) {
        auto& dst = out->entries[i];
        const auto& src = CSR_DEFS[i];
        dst.address = src.address;
        dst.offset = src.offset;
        dst.owner = src.owner;
        dst.implemented = src.implemented ? 1 : 0;

        // TOPOLOGY_MAP (+0x1000) is now built and served by ASFW software
        // (TopologyMapService), not "Planned". Reflect the live ownership so the
        // CSR contract agrees with the Topology Map Valid/DMA Ready fields rather
        // than reporting a stale planned/unimplemented state.
        if (src.offset == 0x1000) {
            if (auto* topoMap = controller_->GetTopologyMapService();
                topoMap && topoMap->IsValid()) {
                dst.owner = ASFWDiagCSROwnerASFWSoftware;
                dst.implemented = 1;
            }
        }
        
        // Copy string safely without std::string overhead
        std::size_t nameLen = std::strlen(src.name);
        if (nameLen >= sizeof(dst.name)) {
            nameLen = sizeof(dst.name) - 1;
        }
        std::memcpy(dst.name, src.name, nameLen);
        dst.name[nameLen] = '\0';

        // Connect counter stats if available
        if (stats) {
            if (src.offset == 0x000) {
                dst.writeCount = stats->inboundStateClearWrites;
            } else if (src.offset == 0x004) {
                dst.writeCount = stats->inboundStateSetWrites;
            } else if (src.offset == 0x21C) {
                dst.readCount = stats->inboundBusManagerIdReads;
                dst.lockCount = stats->inboundBusManagerIdLocks;
            } else if (src.offset == 0x220) {
                dst.readCount = stats->inboundBandwidthReads;
                dst.lockCount = stats->inboundBandwidthLocks;
            } else if (src.offset == 0x224) {
                dst.readCount = stats->inboundChannelReads;
                dst.lockCount = stats->inboundChannelLocks;
            } else if (src.offset == 0x228) {
                dst.readCount = stats->inboundChannelReads;
                dst.lockCount = stats->inboundChannelLocks;
            } else if (src.offset == 0x234) {
                dst.readCount = stats->inboundBroadcastChannelReads;
                dst.writeCount = stats->inboundBroadcastChannelWrites;
            } else if (src.offset == 0x400) {
                dst.readCount = stats->inboundConfigROMReads;
            } else if (src.offset == 0x1000) {
                dst.readCount = stats->inboundTopologyMapReads;
            } else if (src.offset == 0x2000) {
                dst.readCount = stats->inboundSpeedMapReads;
            }
        }
    }

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagCSRContract), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectAsyncTrace(ASFWDiagAsyncTrace* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    auto* trace = controller_->AsyncSubsystem().GetAsyncTraceCapture();
    if (!trace) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagAsyncTrace));
    trace->PopulateSnapshot(out, startGen);

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    // Refresh header timestamp and generation matching consistency rule
    InitHeader(&out->header, sizeof(ASFWDiagAsyncTrace), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectInboundCSRStats(ASFWDiagInboundCSRStats* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    auto* stats = controller_->AsyncSubsystem().GetInboundCSRStats();
    if (!stats) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memcpy(out, stats, sizeof(ASFWDiagInboundCSRStats));

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagInboundCSRStats), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

ASFWDiagStatus DiagnosticsService::CollectBusManager(ASFWDiagBusManager* out) const noexcept {
    if (!controller_ || !out) {
        return ASFWDiagStatusUnavailable;
    }

    const uint32_t startGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;

    std::memset(out, 0, sizeof(ASFWDiagBusManager));
    out->csrContractVerdict = 2; // 0=mismatch, 1=OK, 2=verifier unavailable

    const auto& rolePolicy = controller_->GetRolePolicy();
    out->roleMode = static_cast<uint32_t>(rolePolicy.roleMode);

    auto* hw = controller_->GetHardware();
    if (hw) {
        const uint32_t busOptions = hw->Read(Driver::Register32::kBusOptions);
        auto caps = ASFW::FW::DecodeBusOptions(ASFW::FW::NormalizeLocalBusOptions(
            busOptions, rolePolicy.roleMode, rolePolicy.fullBMActivityLevel));
        out->advertisedBmc = caps.bmc ? 1 : 0;
        out->advertisedIrmc = caps.irmc ? 1 : 0;
        out->advertisedCmc = caps.cmc ? 1 : 0;
        out->advertisedIsc = caps.isc ? 1 : 0;
    }

    const auto& bmState = controller_->GetBusManagerRuntimeState();
    out->localIsIRM = bmState.localIsIRM ? 1 : 0;
    out->localIsBM = bmState.localIsBM ? 1 : 0;
    out->localIsRoot = bmState.localIsRoot ? 1 : 0;
    out->bmOwnerSource = static_cast<uint32_t>(bmState.bmOwnerSource);
    out->lastBusManagerIdOldValue = bmState.lastBusManagerIdOldValue;
    out->staleElectionAbortCount = bmState.staleElectionAbortCount;
    out->failedElectionCount = bmState.failedElectionCount;
    out->unexpectedResourceCsrSoftwareCount = bmState.unexpectedResourceCsrSoftwareCount;

    // Local IRM resource registers & controller status (FW-14 Phase 2)
    auto* const irmCtrl = controller_->GetLocalIRMResourceController();
    if (irmCtrl) {
        auto snap = irmCtrl->Snapshot();
        out->localIrmResourceState = static_cast<uint32_t>(snap.state);
        out->localIrmReadbackValid = snap.activeProbeSucceeded ? 1 : 0;
        out->csrControlLastStatus = static_cast<uint32_t>(snap.lastCsrStatus);
        out->localIrmBusManagerId = snap.busManagerId;
        out->localIrmBandwidthAvailable = snap.bandwidthAvailable;
        out->localIrmChannelsAvailableHi = snap.channelsAvailableHi;
        out->localIrmChannelsAvailableLo = snap.channelsAvailableLo;

        // Milestone 1 additions
        auto* const bc = controller_->GetBroadcastChannel();
        if (bc) {
            out->broadcastChannelValue = bc->Read();
            out->broadcastChannelValid = (out->broadcastChannelValue & 0x40000000U) != 0 ? 1 : 0;
        }

        // Initial registers (from hardware directly)
        auto* const hw = controller_->GetHardware();
        if (hw) {
            using namespace ASFW::Driver;
            out->initialBandwidthAvailable = hw->Read(Register32::kInitialBandwidthAvailable);
            out->initialChannelsAvailableHi = hw->Read(Register32::kInitialChannelsAvailableHi);
            out->initialChannelsAvailableLo = hw->Read(Register32::kInitialChannelsAvailableLo);
        }
    }

    // Topology Map Service status
    auto* topoMap = controller_->GetTopologyMapService();
    if (topoMap) {
        out->topologyMapValid = topoMap->IsValid() ? 1 : 0;
        out->topologyMapCSRGeneration = topoMap->GetGeneration();
        out->topologyMapSelfIdCount = topoMap->GetSelfIdCount();
        out->topologyMapCRC = topoMap->GetCRC();
        out->topologyMapDMAReady = topoMap->IsDMAReady() ? 1 : 0;
    }

    // Populate new diagnostics fields (Pass 1 & 3)
    out->rootCmcKnown = bmState.rootCmcKnown ? 1 : 0;
    out->rootCmcCapable = bmState.rootCmcCapable ? 1 : 0;
    out->cycleStartObserved = bmState.cycleStartObserved ? 1 : 0;
    out->cycleStartSourceNode = bmState.cycleStartSourceNode;
    out->remoteCmstrNeeded = bmState.remoteCmstrNeeded ? 1 : 0;
    out->remoteCmstrAllowed = bmState.remoteCmstrAllowed ? 1 : 0;
    out->remoteCmstrAlreadySatisfied = bmState.remoteCmstrAlreadySatisfied ? 1 : 0;
    out->bmPolicyVerdict = bmState.bmPolicyVerdict;
    out->fullBMActivityLevel = bmState.fullBMActivityLevel;
    out->lastRemoteCmstrResult = bmState.lastRemoteCmstrResult;
    out->lastRemoteCmstrGeneration = bmState.lastRemoteCmstrGeneration;
    out->lastRemoteCmstrTargetNode = bmState.lastRemoteCmstrTargetNode;

    // Milestone 3 additions: Bus Manager Election State
    if (auto* const electDriver = controller_->GetBusManagerElectionDriver()) {
        auto electSnap = electDriver->GetSnapshot();
        out->bmElectionState = electSnap.inFlight ? 1 : 0;
        out->bmElectionResultKind = static_cast<uint32_t>(electDriver->FSM().Owner());
        out->bmElectionLocalFlag = electSnap.wasIncumbent ? 1 : 0;
        out->bmElectionAction = electSnap.lastAction;
        out->bmElectionPath = electSnap.lastElectionPath;
        out->bmElectionCompareValue = 0x3F;
        out->bmElectionSwapValue = electSnap.localNodeId;
        out->bmElectionAttemptedGen = electSnap.attemptedGen;
        out->bmElectionAttemptsThisGen = electSnap.attemptsThisGen;
        
        // Re-classify for display (policy-level)
        out->bmCandidateClass = static_cast<uint32_t>(Bus::Timing::BMCandidateClass::NotCandidate);
        if (rolePolicy.roleMode == ASFW::FW::RoleMode::FullBusManager &&
            rolePolicy.fullBMActivityLevel >= ASFW::FW::FullBMActivityLevel::ElectionOnly) {
            out->bmCandidateClass = static_cast<uint32_t>(electSnap.wasIncumbent
                                                              ? Bus::Timing::BMCandidateClass::Incumbent
                                                              : Bus::Timing::BMCandidateClass::NonIncumbent);
        }
    }

    if (auto* const fallback = controller_->GetIRMFallbackCoordinator()) {
        auto snap = fallback->Snapshot();
        out->irmFallbackState = static_cast<uint32_t>(snap.state);
        out->irmFallbackPlannedAction = static_cast<uint32_t>(snap.plannedAction);
        out->irmFallbackProbeStatus = static_cast<uint32_t>(snap.probeStatus);
        out->irmFallbackRawBusManagerId = snap.busManagerIdRaw;
        out->irmFallbackAnnexHGateOpen = snap.annexHGateOpen ? 1 : 0;
        out->irmFallbackRemainingMs = static_cast<uint32_t>(snap.remainingNs / 1000000ULL);
    }

    if (auto* const cycle = controller_->GetCyclePolicyCoordinator()) {
        auto snap = cycle->Snapshot();
        out->cyclePolicyDecision = static_cast<uint32_t>(snap.lastDecision);
        out->cyclePolicyAction = static_cast<uint32_t>(snap.lastAction);
        out->cyclePolicyTargetNode = snap.targetNode;
        out->cyclePolicyLocalLowLevelMasterBefore = snap.localCycleMasterBefore ? 1 : 0;
        out->cyclePolicyLocalLowLevelMasterAfter = snap.localCycleMasterAfter ? 1 : 0;
        out->cyclePolicyRemoteCmstrInFlight = snap.remoteCmstrInFlight ? 1 : 0;
        out->cyclePolicyRemoteCmstrStatus = snap.remoteCmstrStatus;
        out->cyclePolicyLocalEnableCount = snap.localCycleMasterEnableCount;
        out->cyclePolicyLocalClearCount = snap.localCycleMasterClearCount;
        out->cyclePolicyRemoteSubmitCount = snap.remoteCmstrSubmitCount;
    }

    if (auto* const rootSel = controller_->GetRootSelectionCoordinator()) {
        auto snap = rootSel->Snapshot();
        out->rootSelectionDecision = static_cast<uint32_t>(snap.lastDecision);
        out->rootSelectionAction = static_cast<uint32_t>(snap.lastAction);
        out->rootSelectionSelectedRoot = snap.selectedRoot;
        out->rootSelectionPreviousRoot = snap.previousRoot;
        out->rootSelectionAttemptsThisTopology = snap.attemptsThisTopology;
        out->rootSelectionTotalAttempts = snap.totalAttempts;
        out->rootSelectionRetryLimitHit = snap.retryLimitHit ? 1 : 0;
        out->rootSelectionResetRequested = snap.resetRequested ? 1 : 0;
        out->rootSelectionCurrentGap = snap.currentGapCount;
        out->rootSelectionRequestedGap = snap.requestedGapCount;
    }

    if (auto* const gap = controller_->GetGapPolicyCoordinator()) {
        auto snap = gap->Snapshot();
        out->gapPolicyDecision = static_cast<uint32_t>(snap.lastDecision);
        out->gapPolicyAction = static_cast<uint32_t>(snap.lastAction);
        out->gapPolicyCurrentGap = snap.currentGapCount;
        out->gapPolicyExpectedGap = snap.expectedGapCount;
        out->gapPolicyRequestedGap = snap.requestedGapCount;
        out->gapPolicyComputationSource = static_cast<uint32_t>(snap.computationSource);
        out->gapPolicyMaxHops = snap.maxHopsFromRoot;
        out->gapPolicyMaxHopsKnown = snap.maxHopsKnown ? 1 : 0;
        out->gapPolicyGapConsistent = snap.gapCountConsistent ? 1 : 0;
        out->gapPolicyBetaKnown = snap.betaRepeatersKnown ? 1 : 0;
        out->gapPolicyBetaPresent = snap.betaRepeatersPresent ? 1 : 0;
        out->gapPolicyResetRequested = snap.resetRequested ? 1 : 0;
        out->gapPolicyCombinedWithRootSelection = snap.combinedWithRootSelection ? 1 : 0;
        out->gapPolicyTargetRoot = snap.targetRoot;
        out->gapPolicyAttemptsThisTopology = snap.attemptsThisTopology;
        out->gapPolicyTotalAttempts = snap.totalAttempts;
        out->gapPolicyRetryLimitHit = snap.retryLimitHit;
    }

    if (auto* const power = controller_->GetPowerLinkPolicyCoordinator()) {
        auto snap = power->Snapshot();
        out->powerPolicyDecision = static_cast<uint32_t>(snap.lastDecision);
        out->powerPolicyAction = static_cast<uint32_t>(snap.lastAction);
        out->powerBudgetStatus = static_cast<uint32_t>(snap.powerBudgetStatus);
        out->powerAvailableMilliWatts = snap.powerAvailableMilliWatts;
        out->powerRequiredMilliWatts = snap.powerRequiredMilliWatts;
        out->powerUnknownPowerClassNodes = snap.unknownPowerClassNodes;
        out->powerEligibleNodeCount = snap.eligibleNodeCount;
        out->powerTargetNodeCount = snap.targetNodeCount;
        for (uint32_t i = 0; i < 16; ++i) {
            out->powerTargetNodes[i] = (i < snap.targetNodeCount) ? snap.targetNodes[i] : 0x3F;
        }
        out->linkOnSubmittedCount = snap.linkOnSubmittedCount;
        out->linkOnSuccessCount = snap.linkOnSuccessCount;
        out->linkOnFailureCount = snap.linkOnFailureCount;
        out->linkOnAttemptsThisGeneration = snap.attemptsThisGeneration;
        out->linkOnTotalAttempts = snap.totalAttempts;
    }

    if (auto* const topoService = controller_->GetTopologyMapService()) {
        out->topologyMapPublishStatus = static_cast<uint32_t>(topoService->PublishStatus());
        out->topologyMapGeneration = topoService->GetGeneration();
        out->topologyMapSelfIdCount = topoService->GetSelfIdCount();
        out->topologyMapCRC = topoService->GetCRC();
        out->topologyMapDMAReady = topoService->IsDMAReady() ? 1 : 0;
    }

    if (auto* const speedService = controller_->GetSpeedMapService()) {
        auto snap = speedService->Snapshot();
        out->speedMapStatus = static_cast<uint32_t>(snap.status);
        out->speedMapGeneration = snap.generation;
        out->speedMapNodeCount = snap.nodeCount;
        out->speedMapEncodedQuadlets = snap.encodedLengthQuadlets;
        out->speedMapBetaKnown = 1; // Assuming known if we reached this point
        out->speedMapBetaPresent = snap.betaRepeatersPresent ? 1 : 0;
    }

    if (auto* const responder = controller_->GetCSRResponder()) {
        ASFW::Bus::CSRContractVerifier verifier;
        auto* topo = controller_->GetTopologyMapService();
        auto* speed = controller_->GetSpeedMapService();
        auto* irm = controller_->GetLocalIRMResourceController();

        if (topo && speed && irm) {
            auto check = verifier.Verify(*responder, *topo, *speed, *irm);
            out->csrContractVerdict = check.ok ? 1 : 0;
            out->csrHardwareOwnedSoftwareHits = check.hardwareOwnedSoftwareHits;
            out->csrSoftwareAnsweredHardwareOwned = check.softwareAnsweredHardwareOwned;
            out->csrUnsupportedAccesses = check.unsupportedAccesses;
        }
    }

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagBusManager), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

} // namespace ASFW::Diagnostics
