//
// LinkHandle.cpp — Implementation of ASOHCI → ILink adapter
//

#include "LinkHandle.hpp"
#include "Core/LogHelper.hpp"
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <net.mrmidi.ASFireWire.ASOHCI/ASOHCI.h>
#include <os/log.h>

namespace fw {

// =============================================================================
// Construction & Lifecycle
// =============================================================================

LinkHandle::LinkHandle(OSSharedPtr<ASOHCI> ohci) : _ohci(ohci), _active(true) {
  // Allocate IOLock instances
  _sinkMutex = IOLockAlloc();
  _stateMutex = IOLockAlloc();

  // Initialize cached bus info
  updateCachedBusInfo();

  os_log(ASLog(), "LinkHandle: Created for ASOHCI %p\n", ohci.get());
}

LinkHandle::~LinkHandle() {
  _active.store(false);

  // Clear sink to prevent further event delivery
  {
    IOLockLock(_sinkMutex);
    _sink = nullptr;
    IOLockUnlock(_sinkMutex);
  }

  // Free IOLock instances
  if (_sinkMutex) {
    IOLockFree(_sinkMutex);
    _sinkMutex = nullptr;
  }
  if (_stateMutex) {
    IOLockFree(_stateMutex);
    _stateMutex = nullptr;
  }

  os_log(ASLog(), "LinkHandle: Destroyed\n");
}

// =============================================================================
// ILink Interface Implementation
// =============================================================================

void LinkHandle::setSink(std::weak_ptr<ILinkSink> sink) {
  IOLockLock(_sinkMutex);
  _sink = sink.lock().get(); // Store raw pointer from weak_ptr
  IOLockUnlock(_sinkMutex);

  os_log(ASLog(), "LinkHandle: Sink %s\n", _sink ? "set" : "cleared");
}

BusInfo LinkHandle::getBusInfo() const {
  IOLockLock(_stateMutex);

  // Update cached info if stale
  updateCachedBusInfo();

  BusInfo result = _cachedBusInfo;
  IOLockUnlock(_stateMutex);
  return result;
}

kern_return_t LinkHandle::resetBus(BusResetMode mode) {
  if (!_active.load() || !_ohci) {
    return kIOReturnNotReady;
  }

  bool forceIBR = (mode == BusResetMode::ForceIBR);
  return _ohci->ResetBus(forceIBR);
}

kern_return_t LinkHandle::readQuad(uint16_t nodeID, uint16_t addrHi,
                                   uint32_t addrLo, uint32_t &outValue,
                                   uint32_t atGeneration, Speed speed) {
  if (!_active.load() || !_ohci) {
    return kIOReturnNotReady;
  }

  uint32_t ohciSpeed = convertSpeedToOHCI(speed);
  uint32_t value = 0;

  kern_return_t result =
      _ohci->ReadQuad(nodeID, addrHi, addrLo, &value, atGeneration, ohciSpeed);

  if (result == kIOReturnSuccess) {
    outValue = value;
  }

  return result;
}

void LinkHandle::postToLink(std::function<void()> work) {
  if (!_active.load() || !_ohci || !work) {
    return;
  }

  // Get ASOHCI's default queue and post work to it
  if (auto queue = _ohci->GetDefaultQueue()) {
    auto workBlock = ^{
      if (_active.load()) {
        work();
      }
    };
    queue->DispatchAsync(workBlock);
  } else {
    // Fallback: execute synchronously if no queue available
    if (_active.load()) {
      work();
    }
  }
}

std::string LinkHandle::name() const {
  if (!_ohci) {
    return "LinkHandle(null)";
  }

  // Create descriptive name based on ASOHCI properties
  return "LinkHandle(ASOHCI)";
}

// =============================================================================
// Event Delivery from ASOHCI
// =============================================================================

void LinkHandle::deliverBusReset(uint32_t generation) {
  if (!_active.load()) {
    return;
  }

  // Update cached bus info
  {
    IOLockLock(_stateMutex);
    _cachedBusInfo.generation = generation;
    // Other fields will be updated when getBusInfo() is called
    IOLockUnlock(_stateMutex);
  }

  // Deliver event to controller
  postEventToController([generation](ILinkSink *sink) {
    if (sink)
      sink->onBusReset(generation);
  });
}

void LinkHandle::deliverSelfIDs(const uint32_t *selfIDQuads, uint32_t count,
                                uint32_t generation) {
  if (!_active.load() || !selfIDQuads || count == 0) {
    return;
  }

  // Copy Self-ID data to prevent pointer lifetime issues
  std::vector<uint32_t> copy(selfIDQuads, selfIDQuads + count);

  // Deliver event to controller
  postEventToController([copy = std::move(copy), generation](ILinkSink *sink) {
    if (sink) {
      SelfIDs ids;
      ids.quads = copy.data();
      ids.count = static_cast<uint32_t>(copy.size());
      ids.generation = generation;
      sink->onSelfIDs(ids);
    }
  });
}

void LinkHandle::deliverIsoMasks(uint32_t txMask, uint32_t rxMask) {
  if (!_active.load()) {
    return;
  }

  IsoMask mask;
  mask.txMask = txMask;
  mask.rxMask = rxMask;

  postEventToController([mask](ILinkSink *sink) {
    if (sink)
      sink->onIsoMasks(mask);
  });
}

void LinkHandle::deliverCycleInconsistent(uint32_t cycleTime) {
  if (!_active.load()) {
    return;
  }

  postEventToController([cycleTime](ILinkSink *sink) {
    if (sink)
      sink->onCycleInconsistent(cycleTime);
  });
}

void LinkHandle::deliverPostedWriteError() {
  if (!_active.load()) {
    return;
  }

  postEventToController([](ILinkSink *sink) {
    if (sink)
      sink->onPostedWriteError();
  });
}

void LinkHandle::deliverBusError(uint32_t errorFlags) {
  if (!_active.load()) {
    return;
  }

  postEventToController([errorFlags](ILinkSink *sink) {
    if (sink)
      sink->onBusError(errorFlags);
  });
}

// =============================================================================
// Private Helper Methods
// =============================================================================

void LinkHandle::postEventToController(
    std::function<void(ILinkSink *)> eventCall) {
  if (!_active.load() || !eventCall) {
    return;
  }

  // Get sink safely
  ILinkSink *sink = nullptr;
  {
    IOLockLock(_sinkMutex);
    sink = _sink;
    IOLockUnlock(_sinkMutex);
  }

  if (!sink) {
    return; // No controller connected
  }

  // For MVP, call event directly (controller work queue integration can be
  // added later)
  eventCall(sink);
}

void LinkHandle::updateCachedBusInfo() const {
  if (!_ohci) {
    return;
  }

  // Query current state from ASOHCI
  _cachedBusInfo.localNodeID = _ohci->GetNodeID();
  _cachedBusInfo.localGUID = _ohci->GetLocalGUID();
  _cachedBusInfo.generation = _ohci->GetGeneration();

  // TODO: Get root node ID and max speed from topology
  // These will be available after Self-ID processing

  // For now, use safe defaults
  _cachedBusInfo.rootNodeID = _cachedBusInfo.localNodeID; // Will be updated
  _cachedBusInfo.maxSpeed = Speed::S400; // Conservative default
}

Speed LinkHandle::convertSpeed(uint32_t ohciSpeed) const {
  switch (ohciSpeed) {
  case 0:
    return Speed::S100;
  case 1:
    return Speed::S200;
  case 2:
    return Speed::S400;
  case 3:
    return Speed::S800;
  case 4:
    return Speed::S1600;
  case 5:
    return Speed::S3200;
  default:
    return Speed::S400; // Safe default
  }
}

uint32_t LinkHandle::convertSpeedToOHCI(Speed speed) const {
  switch (speed) {
  case Speed::S100:
    return 0;
  case Speed::S200:
    return 1;
  case Speed::S400:
    return 2;
  case Speed::S800:
    return 3;
  case Speed::S1600:
    return 4;
  case Speed::S3200:
    return 5;
  default:
    return 2; // S400 default
  }
}

} // namespace fw