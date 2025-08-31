//
//  ASFireWireController.cpp
//  ASFireWire Controller - Bus orchestration layer implementation
//
//  Created by ASFireWire MVP - Controller layer separation from ASOHCI
//  Based on CONTROLLER.md architecture and Linux firewire/core-device.c
//  patterns
//

#include <TargetConditionals.h>
#include <os/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOUserServer.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSSharedPtr.h>

// IIG-generated header
#include <net.mrmidi.ASFireWire.ASOHCI/ASFireWireController.h>

// Project headers
#include "ASOHCI.h"

// Forward declarations
struct ASFireWireController_IVars;
static kern_return_t TransitionBusState(ASFireWireController_IVars *ivars,
                                        BusState newState,
                                        const char *description);
static const char *BusStateToString(BusState state);

// Logging helper
static os_log_t ControllerLog() {
  static os_log_t log =
      os_log_create("net.mrmidi.ASFireWire.Controller", "Controller");
  return log;
}

// =====================================================================================
// Controller State Machine and Data Types (CONTROLLER.md §269-302)
// =====================================================================================

enum class BusState : uint32_t {
  Starting = 0,
  WaitingSelfIDs,
  BuildingTopology,
  Scanning,
  Running
};

// Device record for tracking discovered devices (MVP - simplified)
struct DeviceRecord {
  uint16_t nodeID = 0xFFFF;
  uint64_t guid = 0;
  uint32_t generation = 0;
  bool romValid = false;
  uint32_t romQuads[16] = {}; // First 64 bytes of ROM only for MVP
  uint32_t vendorID = 0;
  uint32_t modelID = 0;
  uint32_t specID = 0;
  uint32_t swVersion = 0;
};

// Controller ivars structure (similar pattern to ASOHCI)
struct ASFireWireController_IVars {
  // Link interface
  ASOHCI *link = nullptr;

  // Bus state machine
  uint32_t busState = static_cast<uint32_t>(BusState::Starting);
  char busStateDescription[32] = "Starting";

  // Bus information
  uint32_t generation = 0;
  uint16_t localNodeID = 0xFFFF;
  uint16_t rootNodeID = 0xFFFF;
  uint32_t nodeCount = 0;

  // Self-ID processing
  uint32_t selfIDQuads[256] = {}; // Raw Self-ID data
  uint32_t selfIDCount = 0;

  // Device tracking (MVP - simplified)
  DeviceRecord devices[63] = {}; // Max 63 devices per bus
  uint32_t deviceCount = 0;

  // Dispatch queue for controller operations
  OSSharedPtr<IODispatchQueue> workQueue;

  // State flags
  bool stopping = false;
  uint64_t lastScanTime = 0;
};

// =====================================================================================
// IOService Lifecycle
// =====================================================================================

bool ASFireWireController::init() {
  auto success = super::init();
  if (!success) {
    return false;
  }

  // Allocate ivars
  ivars = IONewZero(ASFireWireController_IVars, 1);
  if (ivars == nullptr) {
    return false;
  }

  // Initialize state machine
  __atomic_store_n(&ivars->busState, static_cast<uint32_t>(BusState::Starting),
                   __ATOMIC_RELEASE);
  strlcpy(ivars->busStateDescription, "Starting",
          sizeof(ivars->busStateDescription));

  os_log(ControllerLog(), "ASFireWireController: init() completed - state: %s",
         ivars->busStateDescription);
  return true;
}

void ASFireWireController::free() {
  os_log(ControllerLog(), "ASFireWireController: free() - current state: %s",
         ivars ? ivars->busStateDescription : "null");

  if (ivars != nullptr) {
    // Set stopping flag
    __atomic_store_n(&ivars->stopping, true, __ATOMIC_RELEASE);

    // Clean up resources
    ivars->workQueue.reset();
    ivars->link = nullptr; // Don't release - we don't own it

    // Clear device records
    memset(ivars->devices, 0, sizeof(ivars->devices));
    ivars->deviceCount = 0;
  }

  // Safe deallocation
  IOSafeDeleteNULL(ivars, ASFireWireController_IVars, 1);
  super::free();
}

// =====================================================================================
// Start/Stop
// =====================================================================================

kern_return_t IMPL(ASFireWireController, Start) {
  kern_return_t kr = Start(provider, SUPERDISPATCH);
  if (kr != kIOReturnSuccess) {
    os_log(ControllerLog(),
           "ASFireWireController: Start superdispatch failed: 0x%08x", kr);
    return kr;
  }

  if (!ivars) {
    os_log(ControllerLog(), "ASFireWireController: ivars not allocated");
    return kIOReturnNoResources;
  }

  os_log(ControllerLog(), "ASFireWireController: Start() begin");

  // Step 1: Get link interface from provider (should be ASOHCI)
  ASOHCI *link = OSDynamicCast(ASOHCI, provider);
  if (!link) {
    os_log(ControllerLog(), "ASFireWireController: Provider is not ASOHCI");
    return kIOReturnBadArgument;
  }

  ivars->link = link;

  // Step 2: Create work queue
  kr = InitializeWorkQueue();
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  // Step 3: Register with link layer
  kr = link->SetController(this);
  if (kr != kIOReturnSuccess) {
    os_log(ControllerLog(),
           "ASFireWireController: SetController failed: 0x%08x", kr);
    return kr;
  }

  // Step 4: Transition to waiting for Self-IDs
  kr = TransitionBusState(ivars, BusState::WaitingSelfIDs, "Start complete");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  os_log(ControllerLog(),
         "ASFireWireController: Start() completed successfully");
  return kIOReturnSuccess;
}

kern_return_t IMPL(ASFireWireController, Stop) {
  if (ivars) {
    __atomic_store_n(&ivars->stopping, true, __ATOMIC_RELEASE);
    TransitionBusState(ivars, BusState::Starting, "Stop");
  }

  os_log(ControllerLog(), "ASFireWireController: Stop completed");

  // Call super Stop
  kern_return_t result = Stop(provider, SUPERDISPATCH);
  return result;
}

// =====================================================================================
// Helper Methods
// =====================================================================================

kern_return_t ASFireWireController::InitializeWorkQueue() {
  kern_return_t kr = ValidateState("InitializeWorkQueue");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  IODispatchQueue *queue = nullptr;
  kr = IODispatchQueue::Create("ASFireWireController.WorkQueue", 0, 0, &queue);
  if (kr != kIOReturnSuccess) {
    os_log(ControllerLog(),
           "ASFireWireController: Failed to create work queue: 0x%08x", kr);
    return kr;
  }

  ivars->workQueue = OSSharedPtr(queue, OSNoRetain);
  os_log(ControllerLog(),
         "ASFireWireController: Work queue created successfully");
  return kIOReturnSuccess;
}

kern_return_t ASFireWireController::ValidateState(const char *operation) {
  if (!ivars) {
    os_log(ControllerLog(), "ASFireWireController: %s - ivars not allocated",
           operation);
    return kIOReturnNoResources;
  }

  if (__atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    os_log(ControllerLog(),
           "ASFireWireController: %s - operation blocked, stopping", operation);
    return kIOReturnNotReady;
  }

  return kIOReturnSuccess;
}

// =====================================================================================
// Bus State Management (CONTROLLER.md §242-243)
// =====================================================================================

kern_return_t ASFireWireController::ResetBus() {
  kern_return_t kr = ValidateState("ResetBus");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  if (!ivars->link) {
    return kIOReturnNoDevice;
  }

  os_log(ControllerLog(), "ASFireWireController: Initiating bus reset");
  return ivars->link->ResetBus(false);
}

kern_return_t ASFireWireController::GetBusInfo(uint32_t *generation,
                                               uint16_t *localNodeID,
                                               uint16_t *rootNodeID) {
  if (!generation || !localNodeID || !rootNodeID) {
    return kIOReturnBadArgument;
  }

  kern_return_t kr = ValidateState("GetBusInfo");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  *generation = ivars->generation;
  *localNodeID = ivars->localNodeID;
  *rootNodeID = ivars->rootNodeID;

  return kIOReturnSuccess;
}

// =====================================================================================
// Device Access - Config ROM Reading (MVP Focus)
// =====================================================================================

kern_return_t ASFireWireController::ReadDeviceROM(uint16_t nodeID,
                                                  uint32_t offset,
                                                  uint32_t *quadlets,
                                                  uint32_t count) {
  if (!quadlets || count == 0) {
    return kIOReturnBadArgument;
  }

  kern_return_t kr = ValidateState("ReadDeviceROM");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  if (!ivars->link) {
    return kIOReturnNoDevice;
  }

  os_log(ControllerLog(),
         "ASFireWireController: ReadDeviceROM nodeID=0x%04x offset=0x%08x "
         "count=%u",
         nodeID, offset, count);

  // Read ROM data via link layer using Config ROM address space (IEEE
  // 1394-2008) Config ROM base address is 0xFFFFF0000400
  for (uint32_t i = 0; i < count; i++) {
    uint16_t addrHi = 0xFFFF;
    uint32_t addrLo = 0xF0000400 + offset + (i * 4);

    kr = ivars->link->ReadQuad(nodeID, addrHi, addrLo, &quadlets[i],
                               ivars->generation, 2 /* S400 */);
    if (kr != kIOReturnSuccess) {
      os_log(ControllerLog(),
             "ASFireWireController: ReadQuad failed at offset %u: 0x%08x", i,
             kr);
      return kr;
    }

    // Check for bus reset during read
    uint32_t currentGen = ivars->link->GetGeneration();
    if (currentGen != ivars->generation) {
      os_log(ControllerLog(),
             "ASFireWireController: Generation changed during ROM read");
      return kIOReturnAborted;
    }
  }

  return kIOReturnSuccess;
}

kern_return_t ASFireWireController::GetDeviceCount(uint32_t *deviceCount) {
  if (!deviceCount) {
    return kIOReturnBadArgument;
  }

  kern_return_t kr = ValidateState("GetDeviceCount");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  *deviceCount = ivars->deviceCount;
  return kIOReturnSuccess;
}

kern_return_t ASFireWireController::GetDeviceInfo(uint32_t deviceIndex,
                                                  DeviceInfo *info) {
  if (!info) {
    return kIOReturnBadArgument;
  }

  kern_return_t kr = ValidateState("GetDeviceInfo");
  if (kr != kIOReturnSuccess) {
    return kr;
  }

  if (deviceIndex >= ivars->deviceCount) {
    return kIOReturnBadArgument;
  }

  // Find the device by scanning the array
  uint32_t found = 0;
  for (uint32_t i = 0; i < 63 && found <= deviceIndex; i++) {
    if (ivars->devices[i].nodeID != 0xFFFF) {
      if (found == deviceIndex) {
        const DeviceRecord &dev = ivars->devices[i];
        info->nodeID = dev.nodeID;
        info->guid = dev.guid;
        info->vendorID = dev.vendorID;
        info->modelID = dev.modelID;
        info->specID = dev.specID;
        info->swVersion = dev.swVersion;
        info->romComplete = dev.romValid;
        return kIOReturnSuccess;
      }
      found++;
    }
  }

  return kIOReturnNotFound;
}

// =====================================================================================
// Event Handlers Called by ASOHCI (PREPARATION.md §257-266)
// =====================================================================================

void ASFireWireController::HandleBusReset(uint32_t generation) {
  if (!ivars || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    return;
  }

  os_log(ControllerLog(), "ASFireWireController: HandleBusReset generation=%u",
         generation);

  ivars->generation = generation;
  ivars->localNodeID = 0xFFFF;
  ivars->rootNodeID = 0xFFFF;
  ivars->nodeCount = 0;

  // Clear device table
  memset(ivars->devices, 0, sizeof(ivars->devices));
  ivars->deviceCount = 0;

  // Transition to waiting for Self-IDs
  TransitionBusState(ivars, BusState::WaitingSelfIDs, "Bus reset");

  // Notify user space
  NotifyBusReset(generation);
}

void ASFireWireController::HandleSelfIDs(const uint32_t *selfIDQuads,
                                         uint32_t count, uint32_t generation) {
  if (!ivars || !selfIDQuads ||
      __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    return;
  }

  BusState currentState = static_cast<BusState>(
      __atomic_load_n(&ivars->busState, __ATOMIC_ACQUIRE));
  if (currentState != BusState::WaitingSelfIDs ||
      ivars->generation != generation) {
    os_log(ControllerLog(), "ASFireWireController: Stale Self-IDs ignored");
    return;
  }

  os_log(ControllerLog(),
         "ASFireWireController: HandleSelfIDs count=%u generation=%u", count,
         generation);

  // Store raw Self-ID data
  uint32_t copyCount = (count < 256) ? count : 256;
  memcpy(ivars->selfIDQuads, selfIDQuads, copyCount * sizeof(uint32_t));
  ivars->selfIDCount = copyCount;

  // Extract basic topology information (simplified for MVP)
  ivars->nodeCount = count; // Simplified - actual count requires parsing
  if (ivars->link) {
    ivars->localNodeID = ivars->link->GetNodeID();
  }

  // Transition to topology building
  TransitionBusState(ivars, BusState::BuildingTopology, "Self-IDs received");

  // Queue topology construction work
  if (ivars->workQueue) {
    ivars->workQueue->DispatchAsync(^{
      BuildTopology();
    });
  }
}

void ASFireWireController::HandleAsyncPacket(const uint32_t *packetData,
                                             uint32_t quadCount,
                                             uint32_t speed) {
  // Not needed for MVP Config ROM reading
  os_log(ControllerLog(),
         "ASFireWireController: HandleAsyncPacket quadCount=%u speed=%u",
         quadCount, speed);
}

// =====================================================================================
// Internal State Machine and Device Management
// =====================================================================================

void ASFireWireController::BuildTopology() {
  if (!ivars || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    return;
  }

  os_log(ControllerLog(),
         "ASFireWireController: BuildTopology - parsing %u Self-ID quadlets",
         ivars->selfIDCount);

  // For MVP, simplified topology building - just extract node count
  // Full implementation would parse Self-ID packets per IEEE 1394-2008
  // §16.3.2.1

  // Assume topology is valid for MVP
  TransitionBusState(ivars, BusState::Scanning, "Topology built");
  StartDeviceScan();
}

void ASFireWireController::StartDeviceScan() {
  if (!ivars || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    return;
  }

  os_log(ControllerLog(),
         "ASFireWireController: StartDeviceScan - scanning %u nodes",
         ivars->nodeCount);

  // For MVP, just scan a few nodes to demonstrate the concept
  // In a full implementation, this would scan all discovered nodes
  for (uint16_t nodeID = 0; nodeID < 4 && nodeID < ivars->nodeCount; nodeID++) {
    if (nodeID == ivars->localNodeID) {
      continue; // Skip local node
    }

    ProcessDeviceROM(nodeID);
  }

  FinalizeBusScan();
}

void ASFireWireController::ProcessDeviceROM(uint16_t nodeID) {
  if (!ivars || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    return;
  }

  os_log(ControllerLog(),
         "ASFireWireController: ProcessDeviceROM nodeID=0x%04x", nodeID);

  // Read basic ROM header (bus info block) - first 5 quadlets
  uint32_t romHeader[5] = {};
  kern_return_t kr = ReadDeviceROM(nodeID, 0, romHeader, 5);
  if (kr != kIOReturnSuccess) {
    os_log(ControllerLog(),
           "ASFireWireController: Failed to read ROM header for node 0x%04x: "
           "0x%08x",
           nodeID, kr);
    return;
  }

  // Extract GUID from bus info block (quadlets 1 and 2)
  uint64_t guid = ((uint64_t)romHeader[1] << 32) | romHeader[2];

  // Extract vendor/model from root directory (simplified)
  uint32_t vendorID = (romHeader[3] >> 8) & 0xFFFFFF; // Simplified extraction
  uint32_t modelID = romHeader[4] & 0xFFFFFF;         // Simplified extraction

  os_log(ControllerLog(),
         "ASFireWireController: Device found - nodeID=0x%04x GUID=0x%016llx "
         "vendor=0x%06x model=0x%06x",
         nodeID, guid, vendorID, modelID);

  // Add to device table
  if (ivars->deviceCount < 63) {
    DeviceRecord &dev = ivars->devices[ivars->deviceCount];
    dev.nodeID = nodeID;
    dev.guid = guid;
    dev.generation = ivars->generation;
    dev.romValid = true;
    dev.vendorID = vendorID;
    dev.modelID = modelID;
    memcpy(dev.romQuads, romHeader, sizeof(romHeader));
    ivars->deviceCount++;

    // Publish device to IORegistry
    PublishDevice(nodeID, guid);
    NotifyDeviceArrived(nodeID, guid);
  }
}

void ASFireWireController::FinalizeBusScan() {
  if (!ivars || __atomic_load_n(&ivars->stopping, __ATOMIC_ACQUIRE)) {
    return;
  }

  os_log(ControllerLog(),
         "ASFireWireController: FinalizeBusScan - found %u devices",
         ivars->deviceCount);

  // Transition to running state
  TransitionBusState(ivars, BusState::Running, "Bus scan complete");

  // Notify topology change
  NotifyTopologyChanged(ivars->generation, ivars->nodeCount);
}

kern_return_t ASFireWireController::PublishDevice(uint16_t nodeID,
                                                  uint64_t guid) {
  // For MVP, just log the device publication
  // Full implementation would create IOFireWireDevice nub
  os_log(ControllerLog(),
         "ASFireWireController: PublishDevice nodeID=0x%04x GUID=0x%016llx",
         nodeID, guid);
  return kIOReturnSuccess;
}

// =====================================================================================
// Event Callbacks for User Space (Future - placeholders for MVP)
// =====================================================================================

void ASFireWireController::NotifyBusReset(uint32_t generation) {
  os_log(ControllerLog(), "ASFireWireController: NotifyBusReset generation=%u",
         generation);
}

void ASFireWireController::NotifyDeviceArrived(uint16_t nodeID, uint64_t guid) {
  os_log(
      ControllerLog(),
      "ASFireWireController: NotifyDeviceArrived nodeID=0x%04x GUID=0x%016llx",
      nodeID, guid);
}

void ASFireWireController::NotifyDeviceDeparted(uint16_t nodeID,
                                                uint64_t guid) {
  os_log(
      ControllerLog(),
      "ASFireWireController: NotifyDeviceDeparted nodeID=0x%04x GUID=0x%016llx",
      nodeID, guid);
}

void ASFireWireController::NotifyTopologyChanged(uint32_t generation,
                                                 uint32_t nodeCount) {
  os_log(
      ControllerLog(),
      "ASFireWireController: NotifyTopologyChanged generation=%u nodeCount=%u",
      generation, nodeCount);
}

// =====================================================================================
// Additional Helper Methods
// =====================================================================================

kern_return_t ASFireWireController::CreateDeviceNub(uint16_t nodeID,
                                                    uint64_t guid,
                                                    uint32_t vendorID,
                                                    uint32_t modelID) {
  // Placeholder for MVP - full implementation would create IOFireWireDevice
  os_log(ControllerLog(), "ASFireWireController: CreateDeviceNub nodeID=0x%04x",
         nodeID);
  return kIOReturnSuccess;
}

bool ASFireWireController::IsDeviceKnown(uint16_t nodeID) {
  for (uint32_t i = 0; i < 63; i++) {
    if (ivars->devices[i].nodeID == nodeID) {
      return true;
    }
  }
  return false;
}

// =====================================================================================
// State Machine Implementation
// =====================================================================================

static kern_return_t TransitionBusState(ASFireWireController_IVars *ivars,
                                        BusState newState,
                                        const char *description) {
  if (!ivars) {
    return kIOReturnNoResources;
  }

  BusState currentState = static_cast<BusState>(
      __atomic_load_n(&ivars->busState, __ATOMIC_ACQUIRE));

  // Perform atomic transition
  __atomic_store_n(&ivars->busState, static_cast<uint32_t>(newState),
                   __ATOMIC_RELEASE);
  strlcpy(ivars->busStateDescription, BusStateToString(newState),
          sizeof(ivars->busStateDescription));

  os_log(
      ControllerLog(), "ASFireWireController: State transition %s -> %s (%s)",
      BusStateToString(currentState), BusStateToString(newState), description);

  return kIOReturnSuccess;
}

static const char *BusStateToString(BusState state) {
  switch (state) {
  case BusState::Starting:
    return "Starting";
  case BusState::WaitingSelfIDs:
    return "WaitingSelfIDs";
  case BusState::BuildingTopology:
    return "BuildingTopology";
  case BusState::Scanning:
    return "Scanning";
  case BusState::Running:
    return "Running";
  default:
    return "Unknown";
  }
}