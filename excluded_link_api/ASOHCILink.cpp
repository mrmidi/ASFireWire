//
//  ASOHCILink.cpp
//  ASOHCI
//
//  Created by Aleksandr Shabelnikov on 30.08.2025.
//

#include "ASOHCILink.h"
#include "ASOHCI.h"
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSSharedPtr.h>
#include <os/log.h>

// Include OHCI constants
#include "OHCIConstants.hpp"

// DriverKit metaclass pattern for ASOHCILinkAPI
extern OSMetaClass *gASOHCILinkAPIMetaClass;

static kern_return_t ASOHCILinkAPI_New(OSMetaClass *instance);

const OSClassLoadInformation ASOHCILinkAPI_Class = {
    .description = nullptr, // Will be set by the system
    .metaPointer = &gASOHCILinkAPIMetaClass,
    .version = 1,
    .instanceSize = sizeof(ASOHCILinkAPI),
    .resv2 = {0},
    .New = &ASOHCILinkAPI_New,
    .resv3 = {0},
};

extern const void *const gASOHCILinkAPI_Declaration;
const void *const gASOHCILinkAPI_Declaration
    __attribute__((used, visibility("hidden"),
                   section("__DATA_CONST,__osclassinfo,regular,no_dead_strip"),
                   no_sanitize("address"))) = &ASOHCILinkAPI_Class;

static kern_return_t ASOHCILinkAPI_New(OSMetaClass *instance) {
  if (!new (instance) ASOHCILinkAPIMetaClass)
    return (kIOReturnNoMemory);
  return (kIOReturnSuccess);
}

class ASOHCILinkAPIMetaClass : public OSMetaClass {
public:
  virtual kern_return_t New(OSObject *instance) override {
    // ASOHCILinkAPI is abstract, so we can't instantiate it directly
    // The concrete subclass ASOHCILink will handle instantiation
    return kIOReturnUnsupported;
  }
};

OSMetaClass *gASOHCILinkAPIMetaClass;

// Include ASOHCI ivars definition
#include "Core/ASOHCIIVars.h"

// Include Topology header
#include "Core/Topology.hpp"

// Include logging helper
#include "LogHelper.hpp"

// Helper function for logging
#define FWLog() ASLog()

// =====================================================================================
// ASOHCILinkAPI Implementation
// =====================================================================================

ASOHCILinkAPI::ASOHCILinkAPI() : OSObject(), fASOHCI(nullptr) {
  // OSObject constructor is called automatically
}

ASOHCILinkAPI::~ASOHCILinkAPI() {
  // OSObject destructor is called automatically
}

// =====================================================================================
// ASOHCILink Implementation
// =====================================================================================

ASOHCILink *ASOHCILink::Create(ASOHCI *owner) {
  if (!owner) {
    return nullptr;
  }

  // Use DriverKit allocation pattern
  ASOHCILink *instance = nullptr;
  kern_return_t ret =
      OSObjectAllocate(gASOHCILinkMetaClass, (OSObject **)&instance);
  if (ret != kIOReturnSuccess || !instance) {
    return nullptr;
  }

  // Initialize the object
  new (instance) ASOHCILink(owner);
  if (!instance->init()) {
    instance->release();
    return nullptr;
  }

  return instance;
}

ASOHCILink::ASOHCILink(ASOHCI *owner) : ASOHCILinkAPI() {
  fASOHCI = owner; // Set the base class member
  os_log(FWLog(), "ASOHCILink: Created with owner %p", owner);
}

ASOHCILink::~ASOHCILink() { os_log(FWLog(), "ASOHCILink: Destroyed"); }

uint64_t ASOHCILink::GetLocalGUID() {
  if (!fASOHCI || !fASOHCI->ivars || !fASOHCI->ivars->pciDevice) {
    os_log(FWLog(), "ASOHCILink: GetLocalGUID - no owner or PCI device");
    return 0;
  }

  uint32_t guidHi = 0, guidLo = 0;
  fASOHCI->ivars->pciDevice->MemoryRead32(fASOHCI->ivars->barIndex,
                                          kOHCI_GUIDHi, &guidHi);
  fASOHCI->ivars->pciDevice->MemoryRead32(fASOHCI->ivars->barIndex,
                                          kOHCI_GUIDLo, &guidLo);

  uint64_t guid = ((uint64_t)guidHi << 32) | guidLo;
  os_log(FWLog(), "ASOHCILink: GetLocalGUID = 0x%016llx", guid);
  return guid;
}

kern_return_t ASOHCILink::ResetBus(bool forceIBR) {
  if (!fASOHCI || !fASOHCI->ivars || !fASOHCI->ivars->pciDevice) {
    os_log(FWLog(), "ASOHCILink: ResetBus - no owner or PCI device");
    return kIOReturnNotReady;
  }

  os_log(FWLog(), "ASOHCILink: ResetBus forceIBR=%d", forceIBR ? 1 : 0);

  // Set the bus reset bit in HCControl
  fASOHCI->ivars->pciDevice->MemoryWrite32(
      fASOHCI->ivars->barIndex, kOHCI_HCControlSet, kOHCI_HCControl_BusReset);

  // If forceIBR is true, also set the InitiateBusReset bit
  if (forceIBR) {
    fASOHCI->ivars->pciDevice->MemoryWrite32(fASOHCI->ivars->barIndex,
                                             kOHCI_HCControlSet,
                                             kOHCI_HCControl_InitiateBusReset);
  }

  return kIOReturnSuccess;
}

uint16_t ASOHCILink::GetNodeID() {
  if (!fASOHCI || !fASOHCI->ivars || !fASOHCI->ivars->pciDevice) {
    os_log(FWLog(), "ASOHCILink: GetNodeID - no owner or PCI device");
    return 0xFFFF;
  }

  uint32_t nodeID = 0;
  fASOHCI->ivars->pciDevice->MemoryRead32(fASOHCI->ivars->barIndex,
                                          kOHCI_NodeID, &nodeID);

  // NodeID format: bits 15:0 contain the node address
  uint16_t nodeAddr = (uint16_t)(nodeID & 0xFFFF);
  os_log(FWLog(), "ASOHCILink: GetNodeID = 0x%04x", nodeAddr);
  return nodeAddr;
}

uint32_t ASOHCILink::GetGeneration() {
  if (!fASOHCI || !fASOHCI->ivars || !fASOHCI->ivars->pciDevice) {
    os_log(FWLog(), "ASOHCILink: GetGeneration - no owner or PCI device");
    return 0;
  }

  uint32_t selfIDCount = 0;
  fASOHCI->ivars->pciDevice->MemoryRead32(fASOHCI->ivars->barIndex,
                                          kOHCI_SelfIDCount, &selfIDCount);

  // Generation is in bits 23:16 of SelfIDCount
  uint32_t generation =
      (selfIDCount & kOHCI_SelfIDCount_selfIDGeneration) >> 16;
  os_log(FWLog(), "ASOHCILink: GetGeneration = %u", generation);
  return generation;
}

kern_return_t ASOHCILink::AsyncRead(uint16_t nodeID, uint32_t addrHi,
                                    uint32_t addrLo, uint32_t length,
                                    uint32_t generation, uint8_t speed) {
  if (!fASOHCI || !fASOHCI->ivars || !fASOHCI->ivars->atManager) {
    os_log(FWLog(), "ASOHCILink: AsyncRead - no AT manager available");
    return kIOReturnNotReady;
  }

  os_log(FWLog(),
         "ASOHCILink: AsyncRead nodeID=0x%04x addr=0x%08x%08x len=%u gen=%u "
         "speed=%u",
         nodeID, addrHi, addrLo, length, generation, speed);

  // For now, return not implemented - would need to implement async read
  // through AT manager
  return kIOReturnUnsupported;
}

kern_return_t ASOHCILink::AsyncWrite(uint16_t nodeID, uint32_t addrHi,
                                     uint32_t addrLo, const void *data,
                                     uint32_t length, uint32_t generation,
                                     uint8_t speed) {
  if (!fASOHCI || !fASOHCI->ivars || !fASOHCI->ivars->atManager) {
    os_log(FWLog(), "ASOHCILink: AsyncWrite - no AT manager available");
    return kIOReturnNotReady;
  }

  os_log(FWLog(),
         "ASOHCILink: AsyncWrite nodeID=0x%04x addr=0x%08x%08x len=%u gen=%u "
         "speed=%u",
         nodeID, addrHi, addrLo, length, generation, speed);

  // For now, return not implemented - would need to implement async write
  // through AT manager
  return kIOReturnUnsupported;
}

bool ASOHCILink::IsRoot() {
  if (!fASOHCI || !fASOHCI->ivars || !fASOHCI->ivars->pciDevice) {
    os_log(FWLog(), "ASOHCILink: IsRoot - no owner or PCI device");
    return false;
  }

  uint32_t nodeID = 0;
  fASOHCI->ivars->pciDevice->MemoryRead32(fASOHCI->ivars->barIndex,
                                          kOHCI_NodeID, &nodeID);

  // Root bit is bit 30 of NodeID register
  bool isRoot = (nodeID & kOHCI_NodeID_root) != 0;
  os_log(FWLog(), "ASOHCILink: IsRoot = %d", isRoot ? 1 : 0);
  return isRoot;
}

uint8_t ASOHCILink::GetNodeCount() {
  if (!fASOHCI || !fASOHCI->ivars || !fASOHCI->ivars->topology) {
    os_log(FWLog(), "ASOHCILink: GetNodeCount - no topology available");
    return 0;
  }

  uint8_t nodeCount = (uint8_t)fASOHCI->ivars->topology->NodeCount();
  os_log(FWLog(), "ASOHCILink: GetNodeCount = %u", nodeCount);
  return nodeCount;
}

void ASOHCILink::SetSelfIDCallback(void (*callback)(void *context),
                                   void *context) {
  if (!fASOHCI || !fASOHCI->ivars) {
    os_log(FWLog(), "ASOHCILink: SetSelfIDCallback - no owner");
    return;
  }

  os_log(FWLog(), "ASOHCILink: SetSelfIDCallback %p context=%p", callback,
         context);

  fASOHCI->ivars->selfIDCallback = callback;
  fASOHCI->ivars->selfIDCallbackContext = context;
}

void ASOHCILink::SetBusResetCallback(void (*callback)(void *context),
                                     void *context) {
  if (!fASOHCI || !fASOHCI->ivars) {
    os_log(FWLog(), "ASOHCILink: SetBusResetCallback - no owner");
    return;
  }

  os_log(FWLog(), "ASOHCILink: SetBusResetCallback %p context=%p", callback,
         context);

  fASOHCI->ivars->busResetCallback = callback;
  fASOHCI->ivars->busResetCallbackContext = context;
}

// =====================================================================================
// ASOHCILink Metaclass Implementation (DriverKit pattern)
// =====================================================================================

// DriverKit metaclass pattern for ASOHCILink
extern OSMetaClass *gASOHCILinkMetaClass;

static kern_return_t ASOHCILink_New(OSMetaClass *instance);

const OSClassLoadInformation ASOHCILink_Class = {
    .description = nullptr, // Will be set by the system
    .metaPointer = &gASOHCILinkMetaClass,
    .version = 1,
    .instanceSize = sizeof(ASOHCILink),
    .resv2 = {0},
    .New = &ASOHCILink_New,
    .resv3 = {0},
};

extern const void *const gASOHCILink_Declaration;
const void *const gASOHCILink_Declaration
    __attribute__((used, visibility("hidden"),
                   section("__DATA_CONST,__osclassinfo,regular,no_dead_strip"),
                   no_sanitize("address"))) = &ASOHCILink_Class;

static kern_return_t ASOHCILink_New(OSMetaClass *instance) {
  if (!new (instance) ASOHCILinkMetaClass)
    return (kIOReturnNoMemory);
  return (kIOReturnSuccess);
}

class ASOHCILinkMetaClass : public OSMetaClass {
public:
  virtual kern_return_t New(OSObject *instance) override {
    // ASOHCILink requires an ASOHCI owner, so we can't instantiate it directly
    // Use ASOHCILink::Create() method instead
    return kIOReturnUnsupported;
  }
};

OSMetaClass *gASOHCILinkMetaClass;