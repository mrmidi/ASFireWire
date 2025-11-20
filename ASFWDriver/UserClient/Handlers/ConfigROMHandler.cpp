//
//  ConfigROMHandler.cpp
//  ASFWDriver
//
//  Handler for Config ROM related UserClient methods
//

#include "ConfigROMHandler.hpp"
#include "ASFWDriver.h"
#include "../../Controller/ControllerCore.hpp"
#include "../../ConfigROM/ConfigROMStore.hpp"
#include "../../ConfigROM/ROMScanner.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/OSData.h>

namespace ASFW::UserClient {

ConfigROMHandler::ConfigROMHandler(ASFWDriver* driver)
    : driver_(driver) {
}

kern_return_t ConfigROMHandler::ExportConfigROM(IOUserClientMethodArguments* args) {
    // Export Config ROM for a given nodeId and generation
    // Input: nodeId[8], generation[16]
    // Output: OSData with ROM quadlets (host byte order)
    if (!args || args->scalarInputCount < 2) {
        return kIOReturnBadArgument;
    }

    const uint8_t nodeId = static_cast<uint8_t>(args->scalarInput[0] & 0xFF);
    const uint16_t generation = static_cast<uint16_t>(args->scalarInput[1] & 0xFFFF);

    ASFW_LOG(UserClient, "ExportConfigROM: nodeId=%u gen=%u", nodeId, generation);

    using namespace ASFW::Driver;
    auto* controller = static_cast<ControllerCore*>(driver_->GetControllerCore());
    if (!controller) {
        ASFW_LOG(UserClient, "ExportConfigROM: controller is NULL");
        return kIOReturnNotReady;
    }

    // Access ConfigROMStore from ControllerCore
    auto* romStore = controller->GetConfigROMStore();
    if (!romStore) {
        ASFW_LOG(UserClient, "ExportConfigROM: romStore is NULL");
        return kIOReturnNotReady;
    }

    uint16_t resolvedGeneration = generation;

    // Lookup ROM by nodeId and generation
    const auto* rom = romStore->FindByNode(generation, nodeId);
    if (!rom) {
        // Fallback: return latest cached ROM for this node (post-reset)
        rom = romStore->FindLatestForNode(nodeId);
        if (rom) {
            resolvedGeneration = rom->gen;
            ASFW_LOG(UserClient,
                     "ExportConfigROM: Requested gen=%u stale, returning latest gen=%u for node=%u",
                     generation, resolvedGeneration, nodeId);
        }
    }

    if (!rom) {
        ASFW_LOG(UserClient,
                 "ExportConfigROM: ROM not found for node=%u gen=%u (no cached fallback)",
                 nodeId, generation);
        // Return empty data to indicate "not cached"
        OSData* data = OSData::withCapacity(0);
        if (!data) return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    // Export raw quadlets (already in host byte order in ConfigROM)
    if (rom->rawQuadlets.empty()) {
        ASFW_LOG(UserClient, "ExportConfigROM: ROM found but rawQuadlets empty");
        OSData* data = OSData::withCapacity(0);
        if (!data) return kIOReturnNoMemory;
        args->structureOutput = data;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    size_t dataSize = rom->rawQuadlets.size() * sizeof(uint32_t);
    OSData* data = OSData::withBytes(rom->rawQuadlets.data(), static_cast<uint32_t>(dataSize));
    if (!data) {
        return kIOReturnNoMemory;
    }

    ASFW_LOG(UserClient,
             "ExportConfigROM: returning %zu quadlets (%zu bytes) for node=%u gen=%u",
             rom->rawQuadlets.size(), dataSize, nodeId, resolvedGeneration);

    if (args->scalarOutput && args->scalarOutputCount >= 1) {
        args->scalarOutput[0] = resolvedGeneration;
        args->scalarOutputCount = 1;
    }

    args->structureOutput = data;
    args->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t ConfigROMHandler::TriggerROMRead(IOUserClientMethodArguments* args) {
    // Manually trigger ROM read for a specific nodeId
    // Input: nodeId[8]
    // Output: status[32] (0=initiated, 1=already_in_progress, 2=failed)
    if (!args || args->scalarInputCount < 1 || args->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    const uint8_t nodeId = static_cast<uint8_t>(args->scalarInput[0] & 0xFF);

    ASFW_LOG(UserClient, "TriggerROMRead: nodeId=%u", nodeId);

    using namespace ASFW::Driver;
    auto* controller = static_cast<ControllerCore*>(driver_->GetControllerCore());
    if (!controller) {
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

    // Trigger ROM read via ROMScanner
    auto* romScanner = controller->GetROMScanner();
    if (!romScanner) {
        ASFW_LOG(UserClient, "TriggerROMRead: romScanner is NULL");
        args->scalarOutput[0] = 2; // failed
        args->scalarOutputCount = 1;
        return kIOReturnError;
    }

    // Request manual ROM read for this node
    bool initiated = romScanner->TriggerManualRead(nodeId, topo->generation, *topo);

    args->scalarOutput[0] = initiated ? 0 : 1; // 0=initiated, 1=already_in_progress
    args->scalarOutputCount = 1;

    ASFW_LOG(UserClient, "TriggerROMRead: nodeId=%u %{public}s",
             nodeId, initiated ? "initiated" : "already in progress");

    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
