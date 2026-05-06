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
#include "../../Logging/Logging.hpp"
#include "ControllerCoreAccess.hpp"

#if __has_include("ASFWDriver.h")
#include "ASFWDriver.h"
#else
// `ASFWDriver.h` is generated from `ASFWDriver/ASFWDriver.iig` by Xcode/IIG.
// Provide a minimal declaration so `clang-tidy` (and non-Xcode builds) can parse this file.
class ASFWDriver {
  public:
    [[nodiscard]] void* GetControllerCore() const;
};
#endif

#include <DriverKit/OSData.h>

namespace ASFW::UserClient {

namespace {

constexpr uint32_t kUnitSpecIdSBP2 = 0x00609E;
constexpr uint32_t kUnitSwVersionSBP2 = 0x010483;

[[nodiscard]] bool HasSBP2Unit(const ASFW::Discovery::ConfigROM& rom) noexcept {
    for (const auto& unit : rom.unitDirectories) {
        if (unit.unitSpecId == kUnitSpecIdSBP2 && unit.unitSwVersion == kUnitSwVersionSBP2) {
            return true;
        }
    }
    return false;
}

} // namespace

ConfigROMHandler::ConfigROMHandler(ASFWDriver* driver) : driver_(driver) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - UserClient argument plumbing.
kern_return_t ConfigROMHandler::ExportConfigROM(IOUserClientMethodArguments* args) {
    // Export Config ROM for a given nodeId and generation
    // Input: nodeId[8], generation[16]
    // Output: OSData with ROM quadlets (wire-order big-endian bytes)
    if (args == nullptr || args->scalarInputCount < 2) {
        return kIOReturnBadArgument;
    }

    const uint8_t nodeId = static_cast<uint8_t>(args->scalarInput[0] & 0xFF);
    const uint16_t generation = static_cast<uint16_t>(args->scalarInput[1] & 0xFFFF);

    const ASFW::Discovery::Generation requestedGen{generation};

    ASFW_LOG(UserClient, "ExportConfigROM: nodeId=%u gen=%u", nodeId, requestedGen.value);

    using namespace ASFW::Driver;
    auto* controller = GetControllerCorePtr(driver_);
    if (controller == nullptr) {
        ASFW_LOG(UserClient, "ExportConfigROM: controller is NULL");
        return kIOReturnNotReady;
    }

    // Access ConfigROMStore from ControllerCore
    auto* romStore = controller->GetConfigROMStore();
    if (romStore == nullptr) {
        ASFW_LOG(UserClient, "ExportConfigROM: romStore is NULL");
        return kIOReturnNotReady;
    }

    ASFW::Discovery::Generation resolvedGen = requestedGen;

    // Lookup ROM by nodeId and generation
    const auto* rom = romStore->FindByNode(requestedGen, nodeId);
    if (rom == nullptr) {
        // Fallback: return latest cached ROM for this node (post-reset)
        rom = romStore->FindLatestForNode(nodeId);
        if (rom != nullptr) {
            resolvedGen = rom->gen;
            ASFW_LOG(UserClient,
                     "ExportConfigROM: Requested gen=%u stale, returning latest gen=%u for node=%u",
                     requestedGen.value, resolvedGen.value, nodeId);
        }
    }

    if (rom == nullptr) {
        ASFW_LOG(UserClient,
                 "ExportConfigROM: ROM not found for node=%u gen=%u (no cached fallback)", nodeId,
                 requestedGen.value);
        // Return empty data to indicate "not cached"
        OSData* data = OSData::withCapacity(0);
        if (data == nullptr) {
            return kIOReturnNoMemory;
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
             "ExportConfigROM: returning %zu quadlets (%zu bytes) for node=%u gen=%u "
             "state=%u rootEntries=%zu unitDirs=%zu hasSBP2=%d requestedGen=%u",
             rom->rawQuadlets.size(), dataSize, nodeId, resolvedGen.value,
             static_cast<uint8_t>(rom->state), rom->rootDirMinimal.size(),
             rom->unitDirectories.size(), HasSBP2Unit(*rom) ? 1 : 0, requestedGen.value);

    if (args->scalarOutput != nullptr && args->scalarOutputCount >= 1) {
        args->scalarOutput[0] = static_cast<uint64_t>(resolvedGen.value & 0xFFFFU);
        args->scalarOutputCount = 1;
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - UserClient argument plumbing.
kern_return_t ConfigROMHandler::TriggerROMRead(IOUserClientMethodArguments* args) {
    // Manually trigger ROM read for a specific nodeId
    // Input: nodeId[8]
    // Output: status[32] (0=initiated, 1=already_in_progress, 2=failed)
    if (args == nullptr || args->scalarInputCount < 1 || args->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    const uint8_t nodeId = static_cast<uint8_t>(args->scalarInput[0] & 0xFF);

    ASFW_LOG(UserClient, "TriggerROMRead: nodeId=%u", nodeId);

    using namespace ASFW::Driver;
    auto* controller = GetControllerCorePtr(driver_);
    if (controller == nullptr) {
        ASFW_LOG(UserClient, "TriggerROMRead: controller is NULL");
        args->scalarOutput[0] = 2; // failed
        args->scalarOutputCount = 1;
        return kIOReturnNotReady;
    }

    // Get current topology to validate nodeId
    auto topo = controller->LatestTopology();
    if (!topo) {
        ASFW_LOG(UserClient, "TriggerROMRead: no topology available");
        args->scalarOutput[0] = 2; // failed
        args->scalarOutputCount = 1;
        return kIOReturnError;
    }

    // Validate nodeId exists in topology
    bool nodeExists = false;
    for (const auto& node : topo->nodes) {
        if (node.nodeId == nodeId) {
            nodeExists = true;
            break;
        }
    }

    if (!nodeExists) {
        ASFW_LOG(UserClient, "TriggerROMRead: nodeId=%u not in topology", nodeId);
        args->scalarOutput[0] = 2; // failed
        args->scalarOutputCount = 1;
        return kIOReturnBadArgument;
    }

    // Trigger ROM scan for this node (callback-driven, single-threaded execution model).
    ASFW::Discovery::ROMScanRequest request{};
    request.gen = ASFW::Discovery::Generation{topo->generation};
    request.topology = *topo;
    request.localNodeId = topo->localNodeId.value_or(0xFF);
    request.targetNodes = {nodeId};

    const bool initiated = controller->StartDiscoveryScan(request);

    args->scalarOutput[0] = initiated ? 0 : 1; // 0=initiated, 1=already_in_progress
    args->scalarOutputCount = 1;

    ASFW_LOG(UserClient, "TriggerROMRead: nodeId=%u %{public}s", nodeId,
             initiated ? "initiated" : "already in progress");

    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
