//
// LinkHandle.hpp — Adapter between OSSharedPtr<ASOHCI> and ILink interface
//
// This class provides the bridge between DriverKit (ASOHCI) and pure C++
// (Controller). It wraps the ASOHCI service with clean RAII semantics and
// thread-safe event delivery. Based on start_that_worked.txt adapter design.
//

#pragma once

#include "ILink.hpp"
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>

// Forward declarations
class ASOHCI;

namespace fw {

// Type alias for shared_ptr to avoid std:: qualification issues in DriverKit
class LinkHandle;
using LinkHandlePtr = std::shared_ptr<LinkHandle>;
using LinkHandleWeakPtr = std::weak_ptr<LinkHandle>;

// =============================================================================
// LinkHandle - RAII Adapter for ASOHCI → ILink
// =============================================================================

class LinkHandle : public ILink,
                   public std::enable_shared_from_this<LinkHandle> {
public:
  // ---- Construction & Lifecycle ----

  // Create adapter wrapping OSSharedPtr<ASOHCI>
  // ASOHCI must be fully initialized before creating LinkHandle
  explicit LinkHandle(OSSharedPtr<ASOHCI> ohci);

  virtual ~LinkHandle();

  // ---- ILink Interface Implementation ----

  void setSink(std::weak_ptr<ILinkSink> sink) override;

  BusInfo getBusInfo() const override;

  kern_return_t resetBus(BusResetMode mode) override;

  kern_return_t readQuad(uint16_t nodeID, uint16_t addrHi, uint32_t addrLo,
                         uint32_t &outValue, uint32_t atGeneration,
                         Speed speed) override;

  void postToLink(std::function<void()> work) override;

  std::string name() const override;

  // ---- Event Delivery from ASOHCI ----

  // These methods are called by ASOHCI interrupt handlers
  // They safely deliver events to the controller's work queue

  // Bus reset detected - called from ASOHCI interrupt context
  void deliverBusReset(uint32_t generation);

  // Self-ID processing complete - called from ASOHCI interrupt context
  void deliverSelfIDs(const uint32_t *selfIDQuads, uint32_t count,
                      uint32_t generation);

  // Isochronous mask update - called from ASOHCI interrupt context
  void deliverIsoMasks(uint32_t txMask, uint32_t rxMask);

  // Cycle timer inconsistency - called from ASOHCI interrupt context
  void deliverCycleInconsistent(uint32_t cycleTime);

  // Posted write error - called from ASOHCI interrupt context
  void deliverPostedWriteError();

  // Unrecoverable bus error - called from ASOHCI interrupt context
  void deliverBusError(uint32_t errorFlags);

  // ---- Internal State Management ----

  // Check if sink is connected and ready for event delivery
  bool hasSink() const;

  // Get weak reference to sink for internal use
  std::weak_ptr<ILinkSink> getSink() const;

private:
  // ---- DriverKit Object Management ----
  OSSharedPtr<ASOHCI> _ohci; // Retains ASOHCI service, no raw pointers

  // ---- Event Sink Management ----
  mutable IOLock *_sinkMutex; // Protects sink pointer
  ILinkSink *_sink; // Raw pointer to controller (lifetime managed externally)

  // ---- Thread Safety & State ----
  std::atomic<bool> _active{true}; // LinkHandle is active for events
  mutable IOLock *_stateMutex;     // Protects cached state

  // Cached bus state for fast getBusInfo() - updated from events
  mutable BusInfo _cachedBusInfo;

  // ---- Event Delivery Helpers ----

  // Post event to controller's work queue safely
  void postEventToController(std::function<void(ILinkSink *)> eventCall);

  // Update cached bus info from ASOHCI state
  void updateCachedBusInfo() const;

  // Convert ASOHCI speed enum to fw::Speed
  Speed convertSpeed(uint32_t ohciSpeed) const;

  // Convert fw::Speed to ASOHCI speed value
  uint32_t convertSpeedToOHCI(Speed speed) const;
};

// =============================================================================
// Inline Implementations for Performance-Critical Methods
// =============================================================================

inline bool LinkHandle::hasSink() const {
  IOLockLock(_sinkMutex);
  bool result = (_sink != nullptr);
  IOLockUnlock(_sinkMutex);
  return result;
}

inline std::weak_ptr<ILinkSink> LinkHandle::getSink() const {
  IOLockLock(_sinkMutex);
  // Return empty weak_ptr since we can't create one from raw pointer
  IOLockUnlock(_sinkMutex);
  return std::weak_ptr<ILinkSink>();
}

} // namespace fw