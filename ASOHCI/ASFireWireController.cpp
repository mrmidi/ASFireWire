//
// ASFireWireController.cpp — Plain C++ Controller Implementation
//
// Bus orchestration layer converted from IOService to pure C++ with RAII.
// Handles topology building, device scanning, and Config ROM parsing.
//

#include "ASFireWireController.hpp"
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <algorithm>
#include <os/log.h>

#include "LogHelper.hpp"

namespace fw {

// =============================================================================
// Static Helpers
// =============================================================================

static const char *busStateToString(BusState state) {
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
  case BusState::Stopping:
    return "Stopping";
  case BusState::Stopped:
    return "Stopped";
  default:
    return "Unknown";
  }
}

// Config ROM parsing helpers
static bool isValidROMHeader(uint32_t quad0) {
  uint8_t info_length = (quad0 >> 24) & 0xFF;
  return (info_length >= 4); // Minimum valid ROM header
}

static uint32_t extractVendorID(const uint32_t *rom, uint32_t quadCount) {
  if (quadCount < 5)
    return 0;
  return rom[4] & 0x00FFFFFF; // Vendor ID in directory entry
}

static uint32_t extractModelID(const uint32_t *rom, uint32_t quadCount) {
  if (quadCount < 6)
    return 0;
  return rom[5] & 0x00FFFFFF; // Model ID typically follows vendor ID
}

// =============================================================================
// Factory & Construction
// =============================================================================

ASFireWireController::Ptr ASFireWireController::create() {
  // Use shared_ptr constructor directly since constructor is private
  auto ptr = std::shared_ptr<ASFireWireController>(new ASFireWireController());
  ptr->_self = ptr; // Store reference to self
  return ptr;
}

ASFireWireController::ASFireWireController() {
  // Allocate IOLock instances
  _stateMutex = IOLockAlloc();
  _busInfoMutex = IOLockAlloc();
  _devicesMutex = IOLockAlloc();
  _selfIDMutex = IOLockAlloc();

  os_log(ASLog(), "ASFireWireController created");
}

ASFireWireController::~ASFireWireController() {
  stop(); // Ensure clean shutdown

  // Free IOLock instances
  if (_stateMutex) {
    IOLockFree(_stateMutex);
    _stateMutex = nullptr;
  }
  if (_busInfoMutex) {
    IOLockFree(_busInfoMutex);
    _busInfoMutex = nullptr;
  }
  if (_devicesMutex) {
    IOLockFree(_devicesMutex);
    _devicesMutex = nullptr;
  }
  if (_selfIDMutex) {
    IOLockFree(_selfIDMutex);
    _selfIDMutex = nullptr;
  }

  os_log(ASLog(), "ASFireWireController destroyed");
}

// =============================================================================
// Controller Lifecycle
// =============================================================================

kern_return_t ASFireWireController::start(ILink::Ptr link) {
  if (!link) {
    os_log(ASLog(), "Cannot start with null link");
    return kIOReturnBadArgument;
  }

  if (isRunning()) {
    os_log(ASLog(), "Controller already running");
    return kIOReturnStillOpen;
  }

  // Store weak reference to link
  _link = link;

  // Create work queue for controller operations
  IODispatchQueue *tempQueue = nullptr;
  kern_return_t result =
      IODispatchQueue::Create("ASFireWireController", 0, 0, &tempQueue);
  if (result != kIOReturnSuccess) {
    os_log(ASLog(), "Failed to create work queue: 0x%{public}x", result);
    return result;
  }
  _workQueue = OSSharedPtr<IODispatchQueue>(tempQueue, OSNoRetain);

  // Register as event sink with link
  link->setSink(std::weak_ptr<ILinkSink>(_self));

  // Initialize state
  transitionState(BusState::WaitingSelfIDs, "Started, waiting for bus reset");

  os_log(ASLog(), "Controller started successfully");
  return kIOReturnSuccess;
}

void ASFireWireController::stop() {
  if (_stopping.exchange(true)) {
    return; // Already stopping
  }

  os_log(ASLog(), "Controller stopping...");
  transitionState(BusState::Stopping, "Stop requested");

  // Clear link connection
  if (auto link = _link.lock()) {
    link->setSink(std::weak_ptr<ILinkSink>());
  }
  _link.reset();

  // Clean up state
  {
    IOLockLock(_devicesMutex);
    _deviceCount = 0;
    std::fill(std::begin(_devices), std::end(_devices), DeviceRecord{});
    IOLockUnlock(_devicesMutex);
  }

  {
    IOLockLock(_selfIDMutex);
    _selfIDCount = 0;
    std::fill(std::begin(_selfIDQuads), std::end(_selfIDQuads), 0);
    IOLockUnlock(_selfIDMutex);
  }

  // Note: Work queue cleanup handled by OSSharedPtr destructor
  transitionState(BusState::Stopped, "Stopped");
  os_log(ASLog(), "Controller stopped");
}

// =============================================================================
// ILinkSink Implementation (Events from Hardware)
// =============================================================================

void ASFireWireController::onBusReset(uint32_t generation) {
  post([this, generation]() { processBusReset(generation); });
}

void ASFireWireController::onSelfIDs(const SelfIDs &ids) {
  post([this, ids]() { processSelfIDs(ids); });
}

void ASFireWireController::onIsoMasks(const IsoMask &mask) {
  // For MVP, just log isochronous mask changes
  os_log(ASLog(), "Iso masks updated: tx=0x%{public}x rx=0x%{public}x",
         mask.txMask, mask.rxMask);
}

void ASFireWireController::onCycleInconsistent(uint32_t cycleTime) {
  os_log(ASLog(), "Cycle inconsistent at time 0x%{public}x", cycleTime);
}

void ASFireWireController::onPostedWriteError() {
  os_log(ASLog(), "Posted write error occurred");
}

void ASFireWireController::onBusError(uint32_t errorFlags) {
  os_log(ASLog(), "Bus error: flags=0x%{public}x", errorFlags);
  // Could transition to error state here if needed
}

// =============================================================================
// Public API for Higher Layers
// =============================================================================

BusInfo ASFireWireController::getBusInfo() const {
  IOLockLock(_busInfoMutex);
  BusInfo result = _cachedBusInfo;
  IOLockUnlock(_busInfoMutex);
  return result;
}

uint32_t ASFireWireController::getDeviceCount() const noexcept {
  IOLockLock(_devicesMutex);
  uint32_t result = _deviceCount;
  IOLockUnlock(_devicesMutex);
  return result;
}

kern_return_t ASFireWireController::getDeviceInfo(uint32_t deviceIndex,
                                                  DeviceInfo &info) const {
  IOLockLock(_devicesMutex);

  if (deviceIndex >= _deviceCount) {
    IOLockUnlock(_devicesMutex);
    return kIOReturnBadArgument;
  }

  const DeviceRecord &device = _devices[deviceIndex];
  info.nodeID = device.nodeID;
  info.guid = device.guid;
  info.vendorID = device.vendorID;
  info.modelID = device.modelID;
  info.specID = device.specID;
  info.swVersion = device.swVersion;
  info.romComplete = device.romValid;

  IOLockUnlock(_devicesMutex);
  return kIOReturnSuccess;
}

kern_return_t ASFireWireController::resetBus() {
  auto link = _link.lock();
  if (!link || !canPerformOperation()) {
    return kIOReturnNotReady;
  }

  return link->resetBus(BusResetMode::Normal);
}

// =============================================================================
// Work Queue Integration
// =============================================================================

void ASFireWireController::post(std::function<void()> work) {
  if (!work || !_workQueue || _stopping.load()) {
    return;
  }

  auto workBlock = ^{
    if (!this->_stopping.load()) {
      work();
    }
  };

  _workQueue->DispatchAsync(workBlock);
}

// =============================================================================
// Event Processing (called on work queue)
// =============================================================================

void ASFireWireController::processBusReset(uint32_t generation) {
  os_log(ASLog(), "Processing bus reset: generation=%u", generation);

  // Update generation and clear device state
  {
    IOLockLock(_busInfoMutex);
    _cachedBusInfo.generation = generation;
    IOLockUnlock(_busInfoMutex);
  }

  {
    IOLockLock(_devicesMutex);
    _deviceCount = 0; // Clear previous devices
    IOLockUnlock(_devicesMutex);
  }

  // Transition state based on current state
  BusState currentState = _state.load();
  if (currentState == BusState::WaitingSelfIDs) {
    // First bus reset - stay in WaitingSelfIDs until Self-IDs arrive
    os_log(ASLog(), "First bus reset received, waiting for Self-IDs");
  } else {
    // Subsequent bus reset - go back to waiting
    transitionState(BusState::WaitingSelfIDs,
                    "Bus reset - waiting for new Self-IDs");
  }
}

void ASFireWireController::processSelfIDs(const SelfIDs &ids) {
  if (!ids.quads || ids.count == 0) {
    os_log(ASLog(), "Invalid Self-IDs received");
    return;
  }

  os_log(ASLog(), "Processing Self-IDs: count=%u generation=%u", ids.count,
         ids.generation);

  // Copy Self-ID data for processing
  {
    IOLockLock(_selfIDMutex);
    _selfIDCount =
        std::min(ids.count, static_cast<uint32_t>(sizeof(_selfIDQuads) /
                                                  sizeof(_selfIDQuads[0])));
    std::copy(ids.quads, ids.quads + _selfIDCount, _selfIDQuads);
    IOLockUnlock(_selfIDMutex);
  }

  transitionState(BusState::BuildingTopology, "Self-IDs received");
  buildTopology();
}

void ASFireWireController::buildTopology() {
  os_log(ASLog(), "Building topology from Self-IDs");

  IOLockLock(_selfIDMutex);
  SelfIDs ids;
  ids.quads = _selfIDQuads;
  ids.count = _selfIDCount;
  ids.generation = _cachedBusInfo.generation;

  // Extract topology information
  uint32_t nodeCount = extractNodeCount(ids);
  uint16_t rootNodeID = extractRootNodeID(ids);
  std::vector<uint16_t> nodeList = extractNodeList(ids);

  IOLockUnlock(_selfIDMutex);

  os_log(ASLog(), "Topology: %u nodes, root=0x%{public}x", nodeCount,
         rootNodeID);

  // Update cached bus info
  auto link = _link.lock();
  if (link) {
    BusInfo linkInfo = link->getBusInfo();
    updateBusInfo(linkInfo.generation, linkInfo.localNodeID, rootNodeID);
  }

  _nodeCount = nodeCount;

  transitionState(BusState::Scanning, "Topology built");
  startDeviceScan();
}

void ASFireWireController::startDeviceScan() {
  os_log(ASLog(), "Starting device scan");

  // For MVP, scan all nodes we discovered
  std::vector<uint16_t> nodeList;
  {
    IOLockLock(_selfIDMutex);
    SelfIDs ids;
    ids.quads = _selfIDQuads;
    ids.count = _selfIDCount;
    ids.generation = _cachedBusInfo.generation;
    nodeList = extractNodeList(ids);
    IOLockUnlock(_selfIDMutex);
  }

  // Start ROM reading for each discovered node
  for (uint16_t nodeID : nodeList) {
    if (nodeID != _cachedBusInfo.localNodeID) { // Don't scan ourselves
      processDeviceROM(nodeID);
    }
  }

  // For now, immediately finalize scan (asynchronous ROM reading not
  // implemented in MVP)
  finalizeBusScan();
}

void ASFireWireController::processDeviceROM(uint16_t nodeID) {
  os_log(ASLog(), "Processing device ROM for node 0x%{public}x", nodeID);

  auto link = _link.lock();
  if (!link) {
    return;
  }

  uint32_t generation = _cachedBusInfo.generation;

  // Read Config ROM header (simplified MVP approach)
  readDeviceROM(nodeID, generation);
}

void ASFireWireController::finalizeBusScan() {
  os_log(ASLog(), "Finalizing bus scan");

  transitionState(BusState::Running, "Device scan complete");

  logState("Bus scan completed");
}

// =============================================================================
// Device Management
// =============================================================================

ASFireWireController::DeviceRecord *
ASFireWireController::findDevice(uint16_t nodeID) {
  for (uint32_t i = 0; i < _deviceCount; ++i) {
    if (_devices[i].nodeID == nodeID) {
      return &_devices[i];
    }
  }
  return nullptr;
}

const ASFireWireController::DeviceRecord *
ASFireWireController::findDevice(uint16_t nodeID) const {
  for (uint32_t i = 0; i < _deviceCount; ++i) {
    if (_devices[i].nodeID == nodeID) {
      return &_devices[i];
    }
  }
  return nullptr;
}

void ASFireWireController::updateDevice(uint16_t nodeID, uint64_t guid,
                                        uint32_t generation) {
  IOLockLock(_devicesMutex);

  DeviceRecord *existing = findDevice(nodeID);
  if (existing) {
    existing->guid = guid;
    existing->generation = generation;
    IOLockUnlock(_devicesMutex);
    return;
  }

  // Add new device if there's space
  if (_deviceCount < MAX_DEVICES) {
    DeviceRecord &device = _devices[_deviceCount++];
    device.nodeID = nodeID;
    device.guid = guid;
    device.generation = generation;
    device.romValid = false;
  }

  IOLockUnlock(_devicesMutex);
}

void ASFireWireController::readDeviceROM(uint16_t nodeID, uint32_t generation) {
  auto link = _link.lock();
  if (!link) {
    return;
  }

  // Read Config ROM outside of locks to prevent deadlocks
  std::array<uint32_t, 16> rom{};
  bool ok = true;
  constexpr uint32_t HI = 0xFFFF;
  constexpr uint32_t LO_BASE = 0xF0000000;

  for (uint32_t i = 0; i < rom.size(); ++i) {
    uint32_t q = 0;
    auto kr = link->readQuad(nodeID, HI, LO_BASE + (i * 4), q, generation,
                             Speed::S400);
    if (kr != kIOReturnSuccess) {
      ok = false;
      break;
    }
    rom[i] = q;

    // Check if generation changed (bus reset occurred)
    if (link->getBusInfo().generation != generation) {
      os_log(ASLog(),
             "Bus reset detected during ROM read for node 0x%{public}x",
             nodeID);
      ok = false;
      break;
    }
  }

  // Update device record after I/O is complete
  IOLockLock(_devicesMutex);
  updateDevice(nodeID, 0, generation);
  if (auto dev = findDevice(nodeID)) {
    if (ok) {
      std::copy(rom.begin(), rom.end(), dev->romQuads.begin());
      parseDeviceROM(*dev, dev->romQuads.data(),
                     static_cast<uint32_t>(rom.size()));
      dev->romValid = true;
    }
  }
  IOLockUnlock(_devicesMutex);
}

void ASFireWireController::parseDeviceROM(DeviceRecord &device,
                                          const uint32_t *romQuads,
                                          uint32_t quadCount) {
  if (!romQuads || quadCount < 4) {
    return;
  }

  // Validate ROM header
  if (!isValidROMHeader(romQuads[0])) {
    return;
  }

  // Extract basic device information (simplified parsing)
  device.vendorID = extractVendorID(romQuads, quadCount);
  device.modelID = extractModelID(romQuads, quadCount);

  // Extract GUID from Config ROM if available
  if (quadCount >= 8) {
    device.guid = (static_cast<uint64_t>(romQuads[6]) << 32) | romQuads[7];
  }

  // Set default values for spec ID and software version
  device.specID = 0x609E;      // IEEE 1394 spec
  device.swVersion = 0x010483; // Default version
}

// =============================================================================
// Utility Methods
// =============================================================================

uint32_t ASFireWireController::extractNodeCount(const SelfIDs &ids) const {
  // Simple node count extraction from Self-IDs
  // Each physical node contributes at least one Self-ID packet
  return std::min(ids.count, static_cast<uint32_t>(63)); // Bus limit
}

uint16_t ASFireWireController::extractRootNodeID(const SelfIDs &ids) const {
  // Root node is typically the highest node ID
  // Simplified implementation - find the highest contiguity
  uint16_t maxNodeID = 0;
  for (uint32_t i = 0; i < ids.count; ++i) {
    uint32_t quad = ids.quads[i];
    if ((quad & 0x40000000) ==
        0) { // More packets bit clear = last packet for this node
      uint16_t nodeID = (quad >> 24) & 0x3F; // Node ID in bits 29-24
      maxNodeID = std::max(maxNodeID, nodeID);
    }
  }
  return maxNodeID;
}

std::vector<uint16_t>
ASFireWireController::extractNodeList(const SelfIDs &ids) const {
  std::vector<uint16_t> nodes;
  nodes.reserve(63); // Bus maximum

  for (uint32_t i = 0; i < ids.count; ++i) {
    uint32_t quad = ids.quads[i];
    if ((quad & 0x40000000) ==
        0) { // More packets bit clear = last packet for this node
      uint16_t nodeID = (quad >> 24) & 0x3F; // Node ID in bits 29-24
      nodes.push_back(nodeID);
    }
  }

  return nodes;
}

void ASFireWireController::updateBusInfo(uint32_t generation,
                                         uint16_t localNodeID,
                                         uint16_t rootNodeID) {
  IOLockLock(_busInfoMutex);
  _cachedBusInfo.generation = generation;
  _cachedBusInfo.localNodeID = localNodeID;
  _cachedBusInfo.rootNodeID = rootNodeID;
  // GUID and maxSpeed should be set from link info
  IOLockUnlock(_busInfoMutex);
}

void ASFireWireController::transitionState(BusState newState,
                                           const std::string &reason) {
  BusState oldState = _state.exchange(newState);

  if (oldState != newState) {
    os_log(ASLog(), "State: %{public}s → %{public}s (%{public}s)",
           busStateToString(oldState), busStateToString(newState),
           reason.c_str());
  }
}

std::string ASFireWireController::stateString() const {
  return busStateToString(_state.load());
}

void ASFireWireController::logState(const std::string &context) const {
  // Lock both mutexes in consistent order: busInfo → devices
  IOLockLock(_busInfoMutex);
  IOLockLock(_devicesMutex);
  os_log(ASLog(),
         "%{public}s: gen=%u local=0x%{public}x root=0x%{public}x devices=%u",
         context.c_str(), _cachedBusInfo.generation, _cachedBusInfo.localNodeID,
         _cachedBusInfo.rootNodeID, _deviceCount);
  IOLockUnlock(_devicesMutex);
  IOLockUnlock(_busInfoMutex);
}

} // namespace fw