//
//  ASFireWireController.cpp
//  ASFireWire
//
//  Created by Aleksandr Shabelnikov on 30.08.2025.
//

#include "ASFireWireController.h"
#include "ASOHCILinkAPI.h"
#include <DriverKit/OSMetaClass.h>
#include <os/log.h>

// Logging
#include "LogHelper.hpp"

OSDefineMetaClassAndStructors(ASFireWireController, OSObject);

ASFireWireController *
ASFireWireController::Create(OSSharedPtr<ASOHCILinkAPI> linkAPI) {
  if (!linkAPI) {
    os_log(ASLog(), "Controller: Cannot create without Link API");
    return nullptr;
  }

  ASFireWireController *controller = new ASFireWireController(linkAPI);
  if (controller && !controller->init()) {
    controller->release();
    return nullptr;
  }

  return controller;
}

ASFireWireController::ASFireWireController(OSSharedPtr<ASOHCILinkAPI> linkAPI)
    : fLinkAPI(linkAPI), fDiscoveryInProgress(false), fCurrentGeneration(0) {}

bool ASFireWireController::init() {
  if (!OSObject::init()) {
    return false;
  }

  os_log(ASLog(), "Controller: Initializing");

  // Set up callbacks
  fLinkAPI->SetSelfIDCallback(SelfIDCallback, this);
  fLinkAPI->SetBusResetCallback(BusResetCallback, this);

  return true;
}

void ASFireWireController::StartDiscovery() {
  if (fDiscoveryInProgress) {
    os_log(ASLog(), "Controller: Discovery already in progress");
    return;
  }

  os_log(ASLog(), "Controller: Starting discovery");

  fDiscoveryInProgress = true;

  // Get local controller info
  uint64_t guid = fLinkAPI->GetLocalGUID();
  os_log(ASLog(), "Controller: Local GUID = 0x%016llx", guid);

  // Force a bus reset to start fresh discovery
  kern_return_t kr = fLinkAPI->ResetBus(false);
  if (kr != kIOReturnSuccess) {
    os_log(ASLog(), "Controller: ResetBus failed: 0x%{public}x", kr);
    fDiscoveryInProgress = false;
    return;
  }

  os_log(ASLog(), "Controller: Bus reset initiated, waiting for Self-ID...");
}

void ASFireWireController::Stop() {
  os_log(ASLog(), "Controller: Stopping");
  fDiscoveryInProgress = false;

  // Clear callbacks
  fLinkAPI->SetSelfIDCallback(nullptr, nullptr);
  fLinkAPI->SetBusResetCallback(nullptr, nullptr);
}

void ASFireWireController::SelfIDCallback(void *context) {
  if (!context)
    return;

  ASFireWireController *controller =
      static_cast<ASFireWireController *>(context);
  controller->HandleSelfIDComplete();
}

void ASFireWireController::BusResetCallback(void *context) {
  if (!context)
    return;

  ASFireWireController *controller =
      static_cast<ASFireWireController *>(context);
  controller->HandleBusReset();
}

void ASFireWireController::HandleSelfIDComplete() {
  if (!fDiscoveryInProgress) {
    return;
  }

  os_log(ASLog(), "Controller: Self-ID complete");

  // Get current bus state
  uint16_t nodeId = fLinkAPI->GetNodeID();
  uint32_t generation = fLinkAPI->GetGeneration();
  bool isRoot = fLinkAPI->IsRoot();
  uint8_t nodeCount = fLinkAPI->GetNodeCount();

  os_log(ASLog(),
         "Controller: NodeID=%u, Generation=%u, IsRoot=%d, NodeCount=%u",
         nodeId, generation, isRoot ? 1 : 0, nodeCount);

  fCurrentGeneration = generation;

  // Start reading Config ROMs from all nodes
  for (uint8_t node = 0; node < nodeCount; node++) {
    if (node != (nodeId & 0x3F)) { // Skip self
      ReadConfigROM(node);
    }
  }

  os_log(ASLog(), "Controller: Discovery phase 1 complete");
}

void ASFireWireController::HandleBusReset() {
  os_log(ASLog(), "Controller: Bus reset detected");

  if (fDiscoveryInProgress) {
    os_log(ASLog(), "Controller: Restarting discovery after bus reset");
    // Bus reset will be followed by Self-ID, so discovery will continue
  }
}

void ASFireWireController::ReadConfigROM(uint16_t nodeID) {
  os_log(ASLog(), "Controller: Reading Config ROM from node %u", nodeID);

  // Try reading Config ROM header (first quadlet)
  // Config ROM base address: 0xFFFF F000 0400
  ASFWAddress configRomAddr(0xFFFF, 0xF0000400, nodeID);

  kern_return_t kr = fLinkAPI->AsyncRead(configRomAddr,
                                         4, // length
                                         fCurrentGeneration,
                                         ASFWSpeed::s400, // S400 speed
                                         nullptr,         // completionContext
                                         nullptr);        // outBuffer

  if (kr != kIOReturnSuccess) {
    os_log(ASLog(),
           "Controller: Failed to read Config ROM header from node %u: 0x%x",
           nodeID, kr);
  } else {
    os_log(ASLog(), "Controller: Config ROM read initiated for node %u",
           nodeID);
    // In a real implementation, you'd handle the async completion
    // and continue reading the full Config ROM
  }
}