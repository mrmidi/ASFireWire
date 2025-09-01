//
// ASFireWireController.hpp — Plain C++ Controller (not IOService)
//
// Bus orchestration layer that handles topology, device scanning, and ROM
// parsing. Converted from IOService to pure C++ with RAII and modern smart
// pointer semantics. Based on start_that_worked.txt design and existing
// controller implementation.
//

#pragma once

#include "ILink.hpp"
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fw {

// =============================================================================
// Forward Declarations & Types
// =============================================================================

struct DeviceInfo {
  uint16_t nodeID = 0xFFFF;
  uint64_t guid = 0;
  uint32_t vendorID = 0;
  uint32_t modelID = 0;
  uint32_t specID = 0;
  uint32_t swVersion = 0;
  bool romComplete = false;
  uint8_t reserved[3] = {}; // Padding for alignment
};

// =============================================================================
// Controller State Machine
// =============================================================================

enum class BusState : uint32_t {
  Starting = 0,     // Initial state, creating work queue
  WaitingSelfIDs,   // Waiting for first bus reset + Self-IDs
  BuildingTopology, // Processing Self-IDs, building topology
  Scanning,         // Scanning devices, reading ROMs
  Running,          // Normal operation, devices discovered
  Stopping,         // Shutdown in progress
  Stopped           // Fully stopped
};

// =============================================================================
// ASFireWireController - Plain C++ Bus Orchestration
// =============================================================================

class ASFireWireController : public ILinkSink {
public:
  using Ptr = std::shared_ptr<ASFireWireController>;
  using WeakPtr = std::weak_ptr<ASFireWireController>;

  // ---- Factory & Lifecycle ----

  // Create controller instance - use this instead of constructor
  static Ptr create();

  virtual ~ASFireWireController();

  // ---- Controller Lifecycle ----

  // Start controller with link - sets up work queue and registers as sink
  [[nodiscard]] kern_return_t start(ILink::Ptr link);

  // Stop controller - drains work queue and cleans up
  void stop();

  // Check if controller is running
  bool isRunning() const noexcept;

  // ---- ILinkSink Implementation (Events from Hardware) ----

  void onBusReset(uint32_t generation) override;
  void onSelfIDs(const SelfIDs &ids) override;
  void onIsoMasks(const IsoMask &mask) override;
  void onCycleInconsistent(uint32_t cycleTime) override;
  void onPostedWriteError() override;
  void onBusError(uint32_t errorFlags) override;

  // ---- Public API for Higher Layers ----

  // Get current bus information snapshot
  BusInfo getBusInfo() const;

  // Get discovered device count
  uint32_t getDeviceCount() const noexcept;

  // Get device information by index
  [[nodiscard]] kern_return_t getDeviceInfo(uint32_t deviceIndex,
                                            DeviceInfo &info) const;

  // Trigger manual bus reset
  [[nodiscard]] kern_return_t resetBus();

  // ---- Work Queue Integration ----

  // Post work to controller's private queue
  void post(std::function<void()> work);

  // Get work queue for external use (LinkHandle needs this)
  OSSharedPtr<IODispatchQueue> getWorkQueue() const;

private:
  // ---- Private Construction ----
  ASFireWireController(); // Use create() instead

  // Store shared_ptr to self for DriverKit compatibility (can't use
  // enable_shared_from_this)
  Ptr _self;

  // ---- Internal State ----

  // Link connection
  ILink::WeakPtr _link; // Weak reference to avoid cycles

  // State management
  std::atomic<BusState> _state{BusState::Starting};
  mutable IOLock *_stateMutex; // Protects complex state operations

  // Bus information (cached)
  mutable IOLock *_busInfoMutex;
  BusInfo _cachedBusInfo;
  uint32_t _nodeCount = 0;

  // Device tracking
  static constexpr size_t MAX_DEVICES = 63; // FireWire bus limit
  mutable IOLock *_devicesMutex;
  struct DeviceRecord {
    uint16_t nodeID = 0xFFFF;
    uint64_t guid = 0;
    uint32_t generation = 0;
    bool romValid = false;
    std::array<uint32_t, 16> romQuads{}; // First 64 bytes for MVP
    uint32_t vendorID = 0;
    uint32_t modelID = 0;
    uint32_t specID = 0;
    uint32_t swVersion = 0;
  };
  DeviceRecord _devices[MAX_DEVICES];
  uint32_t _deviceCount = 0;

  // Self-ID processing
  mutable IOLock *_selfIDMutex;
  uint32_t _selfIDQuads[256] = {}; // Raw Self-ID data storage
  uint32_t _selfIDCount = 0;

  // Work queue for controller operations
  OSSharedPtr<IODispatchQueue> _workQueue;
  std::atomic<bool> _stopping{false};

  // ---- State Machine Implementation ----

  // Transition to new state with logging
  void transitionState(BusState newState, const std::string &reason);

  // Get current state as string for logging
  std::string stateString() const;

  // Validate state for operation
  bool canPerformOperation() const;

  // ---- Event Processing (called on work queue) ----

  // Process bus reset event
  void processBusReset(uint32_t generation);

  // Process Self-ID completion
  void processSelfIDs(const SelfIDs &ids);

  // Build topology from Self-IDs
  void buildTopology();

  // Start device scanning phase
  void startDeviceScan();

  // Process individual device ROM
  void processDeviceROM(uint16_t nodeID);

  // Finalize bus scan and transition to Running
  void finalizeBusScan();

  // ---- Device Management ----

  // Find device record by node ID
  DeviceRecord *findDevice(uint16_t nodeID);
  const DeviceRecord *findDevice(uint16_t nodeID) const;

  // Add or update device record
  void updateDevice(uint16_t nodeID, uint64_t guid, uint32_t generation);

  // Read device Config ROM (MVP implementation)
  void readDeviceROM(uint16_t nodeID, uint32_t generation);

  // Parse ROM quadlets and extract device info
  void parseDeviceROM(DeviceRecord &device, const uint32_t *romQuads,
                      uint32_t quadCount);

  // ---- Utility Methods ----

  // Extract node count from Self-IDs
  uint32_t extractNodeCount(const SelfIDs &ids) const;

  // Extract root node ID from topology
  uint16_t extractRootNodeID(const SelfIDs &ids) const;

  // Convert Self-ID packets to node list
  std::vector<uint16_t> extractNodeList(const SelfIDs &ids) const;

  // Update cached bus info
  void updateBusInfo(uint32_t generation, uint16_t localNodeID,
                     uint16_t rootNodeID);

  // Log controller state for debugging
  void logState(const std::string &context) const;
};

// =============================================================================
// Inline Performance-Critical Methods
// =============================================================================

inline bool ASFireWireController::isRunning() const noexcept {
  BusState state = _state.load();
  return (state != BusState::Starting && state != BusState::Stopping &&
          state != BusState::Stopped);
}

inline bool ASFireWireController::canPerformOperation() const {
  return isRunning() && !_stopping.load();
}

inline OSSharedPtr<IODispatchQueue> ASFireWireController::getWorkQueue() const {
  return _workQueue;
}

} // namespace fw