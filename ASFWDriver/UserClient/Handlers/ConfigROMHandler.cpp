//
//  ConfigROMHandler.cpp
//  ASFWDriver
//
//  Handler for Config ROM related UserClient methods
//

#include "ConfigROMHandler.hpp"
#include "../../ConfigROM/ConfigROMStore.hpp"
#include "../../ConfigROM/ROMScanner.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"
#include "ControllerCoreAccess.hpp"

#if __has_include("ASFWDriver.h")
#include "ASFWDriver.h"
#else
// `ASFWDriver.h` is generated from `ASFWDriver/ASFWDriver.iig` by Xcode/IIG.
// Provide a minimal declaration so `clang-tidy` (and non-Xcode builds) can parse this file.
namespace ASFW::Driver {
class ControllerCore;
}
class ASFWDriver {
  public:
    [[nodiscard]] void* GetControllerCore() const;
};
#endif

#include <DriverKit/OSData.h>

namespace ASFW::UserClient {

ConfigROMHandler::ConfigROMHandler(ASFWDriver* driver) : driver_(driver) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - UserClient argument plumbing.
kern_return_t ConfigROMHandler::ExportConfigROM(IOUserClientMethodArguments* args) {
    // Export the cached Config ROM for one runtime physical instance.
    // Input: DeviceInstanceId[64]
    // Output: OSData with ROM quadlets (wire-order big-endian bytes)
    if (args == nullptr || args->scalarInputCount < 1) {
        return kIOReturnBadArgument;
    }

    const ASFW::Discovery::DeviceInstanceId instanceId{args->scalarInput[0]};
    if (!instanceId) {
        return kIOReturnBadArgument;
    }

    using namespace ASFW::Driver;
    auto* controller = GetControllerCorePtr(driver_);
    if (controller == nullptr) {
        ASFW_LOG(UserClient, "ExportConfigROM: controller is NULL");
        return kIOReturnNotReady;
    }

    auto* registry = controller->GetDeviceRegistry();
    auto* romStore = controller->GetConfigROMStore();
    if (registry == nullptr || romStore == nullptr) {
        ASFW_LOG(UserClient, "ExportConfigROM: identity or ROM store unavailable");
        return kIOReturnNotReady;
    }

    // Config-ROM evidence remains readable for quarantined devices, but a
    // suspended/retired instance has no current node with which to index the
    // generation-scoped store.
    const auto device = registry->Snapshot(instanceId);
    if (!device.has_value()) {
        return kIOReturnNotFound;
    }
    const auto nodeId = ASFW::Discovery::TryOperationalNodeId(device->nodeId);
    if (!nodeId.has_value()) {
        return kIOReturnNotReady;
    }
    const auto requestedGen = device->gen;
    ASFW_LOG(UserClient, "ExportConfigROM: instance=%llu node=%u gen=%u",
             instanceId.value, *nodeId, requestedGen.value);

    const auto rom = romStore->FindByNode(requestedGen, *nodeId);

    if (!rom.has_value()) {
        ASFW_LOG(UserClient,
                 "ExportConfigROM: ROM not found for node=%u gen=%u (exact-generation lookup "
                 "only; no stale fallback)",
                 *nodeId, requestedGen.value);
        // Return empty data to indicate "not cached"
        OSData* data = OSData::withCapacity(0);
        if (data == nullptr) {
            return kIOReturnNoMemory;
        }
        if (args->scalarOutput != nullptr && args->scalarOutputCount >= 1) {
            args->scalarOutput[0] = static_cast<uint64_t>(requestedGen.value & 0xFFFFU);
            args->scalarOutputCount = 1;
        }
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    // Export raw quadlets (stored as byte-exact wire-order big-endian)
    if (rom->rawQuadlets.empty()) {
        ASFW_LOG(UserClient, "ExportConfigROM: ROM found but rawQuadlets empty");
        OSData* data = OSData::withCapacity(0);
        if (data == nullptr) {
            return kIOReturnNoMemory;
        }
        if (args->scalarOutput != nullptr && args->scalarOutputCount >= 1) {
            args->scalarOutput[0] = static_cast<uint64_t>(requestedGen.value & 0xFFFFU);
            args->scalarOutputCount = 1;
        }
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    size_t dataSize = rom->rawQuadlets.size() * sizeof(uint32_t);
    OSData* data = OSData::withBytes(rom->rawQuadlets.data(), static_cast<uint32_t>(dataSize));
    if (data == nullptr) {
        return kIOReturnNoMemory;
    }

    ASFW_LOG(UserClient,
             "ExportConfigROM: returning %zu quadlets (%zu bytes) for node=%u gen=%u",
             rom->rawQuadlets.size(), dataSize, *nodeId, requestedGen.value);

    if (args->scalarOutput != nullptr && args->scalarOutputCount >= 1) {
        args->scalarOutput[0] = static_cast<uint64_t>(requestedGen.value & 0xFFFFU);
        args->scalarOutputCount = 1;
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - UserClient argument plumbing.
kern_return_t ConfigROMHandler::TriggerROMRead(IOUserClientMethodArguments* args) {
    // Manually trigger a Config-ROM-only read for a physical runtime instance.
    // Input: DeviceInstanceId[64]
    // Output: status[32] (0=initiated, 1=already_in_progress, 2=failed)
    if (args == nullptr || args->scalarInputCount < 1 || args->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    const ASFW::Discovery::DeviceInstanceId instanceId{args->scalarInput[0]};
    if (!instanceId) {
        return kIOReturnBadArgument;
    }

    using namespace ASFW::Driver;
    auto* controller = GetControllerCorePtr(driver_);
    if (controller == nullptr) {
        ASFW_LOG(UserClient, "TriggerROMRead: controller is NULL");
        args->scalarOutput[0] = 2; // failed
        args->scalarOutputCount = 1;
        return kIOReturnNotReady;
    }

    auto* registry = controller->GetDeviceRegistry();
    if (registry == nullptr) {
        args->scalarOutput[0] = 2;
        args->scalarOutputCount = 1;
        return kIOReturnNotReady;
    }
    const auto device = registry->Snapshot(instanceId);
    if (!device.has_value()) {
        args->scalarOutput[0] = 2;
        args->scalarOutputCount = 1;
        return kIOReturnNotFound;
    }
    const auto nodeId = ASFW::Discovery::TryOperationalNodeId(device->nodeId);
    if (!nodeId.has_value()) {
        args->scalarOutput[0] = 2;
        args->scalarOutputCount = 1;
        return kIOReturnNotReady;
    }

    ASFW_LOG(UserClient, "TriggerROMRead: instance=%llu node=%u gen=%u",
             instanceId.value, *nodeId, device->gen.value);

    // Get current topology to validate the registry binding.
    auto topo = controller->LatestTopology();
    if (!topo) {
        ASFW_LOG(UserClient, "TriggerROMRead: no topology available");
        args->scalarOutput[0] = 2; // failed
        args->scalarOutputCount = 1;
        return kIOReturnError;
    }
    if (device->gen.value != topo->generation) {
        ASFW_LOG(UserClient,
                 "TriggerROMRead: stale route instance=%llu routeGen=%u topologyGen=%u",
                 instanceId.value, device->gen.value, topo->generation);
        args->scalarOutput[0] = 2;
        args->scalarOutputCount = 1;
        return kIOReturnAborted;
    }

    // Validate nodeId exists in topology
    bool nodeExists = false;
    for (const auto& node : topo->physical.nodes) {
        if (node.physicalId == *nodeId) {
            nodeExists = true;
            break;
        }
    }

    if (!nodeExists) {
        ASFW_LOG(UserClient, "TriggerROMRead: instance=%llu nodeId=%u not in topology",
                 instanceId.value, *nodeId);
        args->scalarOutput[0] = 2; // failed
        args->scalarOutputCount = 1;
        return kIOReturnBadArgument;
    }

    // Trigger ROM scan for this node (callback-driven, single-threaded execution model).
    ASFW::Discovery::ROMScanRequest request{};
    request.gen = ASFW::Discovery::Generation{topo->generation};
    request.topology = *topo;
    request.localNodeId = topo->localNodeId;
    request.targetNodes = {*nodeId};

    if (auto* romStore = controller->GetConfigROMStore(); romStore != nullptr) {
        if (const auto cached = romStore->FindByNode(request.gen, *nodeId, true);
            cached.has_value()) {
            ASFW_LOG(UserClient,
                     "TriggerROMRead: invalidating exact-generation cached ROM for nodeId=%u "
                     "cachedGen=%u guid=0x%016llx before manual reread",
                     *nodeId, cached->gen.value, cached->bib.guid);
            romStore->InvalidateNode(request.gen, *nodeId);
            romStore->PruneInvalid();
        }
    }

    const bool initiated = controller->StartDiscoveryScan(request);

    args->scalarOutput[0] = initiated ? 0 : 1; // 0=initiated, 1=already_in_progress
    args->scalarOutputCount = 1;

    ASFW_LOG(UserClient, "TriggerROMRead: instance=%llu nodeId=%u %{public}s",
             instanceId.value, *nodeId,
             initiated ? "initiated" : "already in progress");

    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
