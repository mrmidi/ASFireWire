#include "DiagnosticsService.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../Bus/BusManager.hpp"
#include "../Async/AsyncSubsystem.hpp"
#include "../Async/Interfaces/IAsyncSubsystemPort.hpp"
#include "../Debug/AsyncTraceCapture.hpp"
#include "../Bus/CSR/TopologyMapService.hpp"
#include <cstring>
#include <DriverKit/IOLib.h>

namespace ASFW::Diagnostics {

namespace {

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
        out->localNode = topo.localNodeId.value_or(0xFF);
        out->rootNode = topo.rootNodeId.value_or(0xFF);
        out->irmNode = topo.irmNodeId.value_or(0xFF);
        out->bmNode = topo.irmNodeId.value_or(0xFF); // IRM acts as BM in most topologies
        out->nodeCount = topo.nodeCount;
        out->gapCount = topo.gapCount;
        out->maxHops = topo.maxHopsFromRoot;
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
        out->localNode = topo.localNodeId.value_or(0xFF);
        out->rootNode = topo.rootNodeId.value_or(0xFF);
        out->irmNode = topo.irmNodeId.value_or(0xFF);
        out->nodeCount = topo.nodeCount;
        
        // Copy raw Self-IDs
        const auto& rawQuads = topo.selfIDData.rawQuadlets;
        out->rawSelfIdCount = static_cast<uint32_t>(rawQuads.size());
        const uint32_t copyCount = (out->rawSelfIdCount > ASFW_DIAG_MAX_SELF_ID_QUADS) 
                                       ? ASFW_DIAG_MAX_SELF_ID_QUADS 
                                       : out->rawSelfIdCount;
        for (uint32_t i = 0; i < copyCount; ++i) {
            out->rawSelfIds[i] = rawQuads[i];
        }
        out->selfIdSequenceCount = static_cast<uint32_t>(topo.selfIDData.sequences.size());
        out->enumeratorError = topo.selfIDData.valid ? 0 : 1;

        // Copy decoded nodes
        const uint32_t copyNodes = (topo.nodes.size() > ASFW_DIAG_MAX_NODES) 
                                       ? ASFW_DIAG_MAX_NODES 
                                       : static_cast<uint32_t>(topo.nodes.size());
        for (uint32_t i = 0; i < copyNodes; ++i) {
            const auto& srcNode = topo.nodes[i];
            auto& dstNode = out->nodes[i];
            dstNode.nodeId = srcNode.nodeId;
            dstNode.linkActive = srcNode.linkActive ? 1 : 0;
            dstNode.contender = srcNode.isIRMCandidate ? 1 : 0;
            dstNode.speed = MbpsToDiagSpeed(srcNode.maxSpeedMbps);
            dstNode.powerClass = srcNode.powerClass;
            dstNode.gapCount = srcNode.gapCount;
            dstNode.portCount = srcNode.portCount;
            dstNode.isLocal = (topo.localNodeId && srcNode.nodeId == *topo.localNodeId) ? 1 : 0;
            dstNode.isRoot = (topo.rootNodeId && srcNode.nodeId == *topo.rootNodeId) ? 1 : 0;
            dstNode.isIRM = (topo.irmNodeId && srcNode.nodeId == *topo.irmNodeId) ? 1 : 0;
            dstNode.initiatedReset = srcNode.initiatedReset ? 1 : 0;
            
            const uint32_t copyPorts = (srcNode.portStates.size() > ASFW_DIAG_MAX_PORTS) 
                                           ? ASFW_DIAG_MAX_PORTS 
                                           : static_cast<uint32_t>(srcNode.portStates.size());
            for (uint32_t p = 0; p < copyPorts; ++p) {
                dstNode.ports[p] = static_cast<uint32_t>(srcNode.portStates[p]);
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

    // Get statistics to populate CSR access counts
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
    // IRM resource registers (BUS_MANAGER_ID/BANDWIDTH/CHANNELS) are serviced by the OHCI
    // CSR engine; TOPOLOGY_MAP/SPEED_MAP are not OHCI-served and are not yet implemented here.
    static const LocalCSREntryDef CSR_DEFS[] = {
        { 0xFFFFF0000000ULL, 0x000, ASFWDiagCSROwnerASFWSoftware,         true,  "STATE_CLEAR" },
        { 0xFFFFF0000004ULL, 0x004, ASFWDiagCSROwnerASFWSoftware,         true,  "STATE_SET" },
        { 0xFFFFF000021CULL, 0x21C, ASFWDiagCSROwnerOHCIHardware,         true,  "BUS_MANAGER_ID" },
        { 0xFFFFF0000220ULL, 0x220, ASFWDiagCSROwnerOHCIHardware,         true,  "BANDWIDTH_AVAILABLE" },
        { 0xFFFFF0000224ULL, 0x224, ASFWDiagCSROwnerOHCIHardware,         true,  "CHANNELS_AVAILABLE_HI" },
        { 0xFFFFF0000228ULL, 0x228, ASFWDiagCSROwnerOHCIHardware,         true,  "CHANNELS_AVAILABLE_LO" },
        { 0xFFFFF0000234ULL, 0x234, ASFWDiagCSROwnerASFWSoftware,         true,  "BROADCAST_CHANNEL" },
        { 0xFFFFF0000400ULL, 0x400, ASFWDiagCSROwnerOHCIHardware,         true,  "CONFIG_ROM" },
        { 0xFFFFF0001000ULL, 0x1000, ASFWDiagCSROwnerPlanned,             false, "TOPOLOGY_MAP" },
        { 0xFFFFF0002000ULL, 0x2000, ASFWDiagCSROwnerOmittedAddressError, false, "SPEED_MAP" }
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

    const auto& config = controller_->GetConfig();
    out->roleMode = static_cast<uint32_t>(config.roleMode);

    auto* hw = controller_->GetHardware();
    if (hw) {
        const uint32_t busOptions = hw->Read(Driver::Register32::kBusOptions);
        auto caps = ASFW::FW::DecodeBusOptions(ASFW::FW::NormalizeLocalBusOptions(busOptions, config.roleMode, config.fullBMActivityLevel));
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
        out->localIrmReadbackValid = snap.readbackValid ? 1 : 0;
        out->csrControlLastStatus = static_cast<uint32_t>(snap.lastCsrStatus);
        
        out->localIrmBusManagerId = snap.busManagerId;
        out->localIrmBandwidthAvailable = snap.bandwidthAvailable;
        out->localIrmChannelsAvailableHi = snap.channelsAvailableHi;
        out->localIrmChannelsAvailableLo = snap.channelsAvailableLo;
    }

    // Topology Map Service status
    auto* topoMap = controller_->GetTopologyMapService();
    if (topoMap) {
        out->topologyMapValid = topoMap->IsValid() ? 1 : 0;
        out->topologyMapGeneration = topoMap->GetGeneration();
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

    const uint32_t endGen = controller_->AsyncSubsystem().GetBusStateSnapshot().generation16;
    if (startGen != endGen) {
        return ASFWDiagStatusStaleGeneration;
    }

    InitHeader(&out->header, sizeof(ASFWDiagBusManager), endGen, snapshotSeq_++);
    return ASFWDiagStatusOK;
}

} // namespace ASFW::Diagnostics
