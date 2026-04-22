#include "ControllerCore.hpp"

#include <DriverKit/IOLib.h>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "../Async/DMAMemoryImpl.hpp"
#include "../Async/FireWireBusImpl.hpp"
#include "../Bus/BusResetCoordinator.hpp"
#include "../Bus/SelfIDCapture.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../ConfigROM/ConfigROMBuilder.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ConfigROMStore.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Diagnostics/DiagnosticLogger.hpp"
#include "../Diagnostics/MetricsSink.hpp"
#include "../Discovery/DeviceManager.hpp"
#include "../Discovery/DeviceRegistry.hpp"
#include "../Discovery/SpeedPolicy.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Hardware/IEEE1394.hpp"
#include "../Hardware/InterruptManager.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "../Hardware/OHCIEventCodes.hpp"
#include "../Hardware/RegisterMap.hpp"
#include "../IRM/IRMClient.hpp"
#include "../Protocols/AVC/AVCDiscovery.hpp"
#include "../Protocols/AVC/CMP/CMPClient.hpp"
#include "../Protocols/Audio/DeviceProtocolFactory.hpp"
#include "../Protocols/SBP2/SBP2SessionRegistry.hpp"
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
    if (!deps_.romScanner) {
        ASFW_LOG(Discovery, "OnTopologyReady: no ROMScanner available");
        return;
    }

    const uint8_t localNodeId = snap.localNodeId.value_or(0xFF);
    if (localNodeId == 0xFF) {
        ASFW_LOG(Discovery, "OnTopologyReady: invalid local node ID");
        return;
    }

    // Count how many nodes will actually be scanned (exclude local + link-inactive)
    uint32_t scannableCount = 0;
    for (const auto& node : snap.nodes) {
        if (node.nodeId != localNodeId && node.linkActive) {
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

    const bool started = StartDiscoveryScan(request);
    if (!started) {
        ASFW_LOG(Discovery, "OnTopologyReady: ROMScanner already busy for gen=%u", snap.generation);
    }

    if (deps_.irmClient) {
        const uint8_t irmNodeId = snap.irmNodeId.value_or(0xFF);
        deps_.irmClient->SetIRMNode(irmNodeId, Discovery::Generation{snap.generation});
        ASFW_LOG(Discovery, "IRMClient updated: IRM node=%u, generation=%u", irmNodeId,
                 snap.generation);
    }

    if (deps_.cmpClient) {
        // CMP (PCR) operations target a *specific device's* plug registers, not the IRM node.
        // Device-scoped CMP wiring is done at stream start time (IsochService).
    }

    if (deps_.sbp2SessionRegistry) {
        deps_.sbp2SessionRegistry->OnBusReset(static_cast<uint16_t>(snap.generation));
    }

    // NOTE: CSR STATE_SET CMSTR write removed. Apple IOFireWireFamily does NOT write
    // CSR STATE_SET via async transactions — it uses the OHCI LinkControl register
    // directly (kCycleMaster bit), which ASFWDriver already sets in kDefaultLinkControl
    // during controller initialization. Async loopback to the local node does not work
    // in ASFWDriver (always returns timeout), so the previous implementation was a no-op.
    // OHCI hardware generates cycle-start packets automatically when the node is root
    // and kCycleMaster is set in LinkControl.
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
    std::unordered_set<uint64_t> discoveredGuids;
    discoveredGuids.reserve(roms.size());

    for (const auto& rom : roms) {
        deps_.romStore->Insert(rom);

        auto policy = deps_.speedPolicy->ForNode(rom.nodeId);

        auto& bus = this->Bus();
        auto& deviceRecord = deps_.deviceRegistry->UpsertFromROM(
            rom, policy, static_cast<Async::IFireWireBusOps*>(&bus),
            static_cast<Async::IFireWireBusInfo*>(&bus));
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

    if (deps_.deviceManager) {
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
    }

    ASFW_LOG(Discovery, "═══════════════════════════════════════");
    ASFW_LOG(Discovery, "Discovery complete: %zu devices processed in gen=%u", roms.size(),
             gen.value);
    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");

    if (deps_.sbp2SessionRegistry) {
        deps_.sbp2SessionRegistry->RefreshTargets(gen);
    }
}

} // namespace ASFW::Driver
