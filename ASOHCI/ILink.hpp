//
// ILink.hpp — Pure C++ interface for FireWire link abstraction
//
// This interface provides clean separation between hardware (ASOHCI) and
// bus orchestration (Controller). Zero raw pointers, full RAII.
// Based on start_that_worked.txt design specification.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// DriverKit types
#include <DriverKit/IOReturn.h>

namespace fw {

// Forward declarations
struct ILinkSink;

// =============================================================================
// Core Types and Enums
// =============================================================================

enum class Speed : uint8_t {
  S100 = 0,
  S200 = 1,
  S400 = 2,
  S800 = 3,
  S1600 = 4,
  S3200 = 5
};

enum class BusResetMode : uint8_t {
  Normal = 0,  // Standard bus reset
  ForceIBR = 1 // Force immediate bus reset
};

// Bus state snapshot - small struct, safe to copy
struct BusInfo {
  uint32_t generation = 0;       // Current bus generation
  uint16_t localNodeID = 0xFFFF; // Our node ID
  uint16_t rootNodeID = 0xFFFF;  // Root node ID
  uint64_t localGUID = 0;        // Our GUID
  Speed maxSpeed = Speed::S400;  // Bus maximum speed
};

// Self-ID packet view - read-only, valid only during callback scope
struct SelfIDs {
  const uint32_t *quads = nullptr; // Self-ID quadlets (read-only view)
  uint32_t count = 0;              // Number of quadlets
  uint32_t generation = 0;         // Generation these Self-IDs belong to
};

// Isochronous channel mask - for future iso support
struct IsoMask {
  uint32_t txMask = 0; // Transmit channels available
  uint32_t rxMask = 0; // Receive channels available
};

// =============================================================================
// ILink Interface - Hardware abstraction for Controller
// =============================================================================

struct ILink {
  using Ptr = std::shared_ptr<ILink>;
  using WeakPtr = std::weak_ptr<ILink>;

  virtual ~ILink() = default;

  // ---- Lifecycle & Event Wiring ----

  // Register controller as event sink
  // Controller provides weak_ptr to avoid ownership cycles
  virtual void setSink(std::weak_ptr<ILinkSink> sink) = 0;

  // ---- Bus State Queries (fast, thread-safe) ----

  // Get current bus state snapshot
  virtual BusInfo getBusInfo() const = 0;

  // ---- Bus Control Operations ----

  // Initiate bus reset
  virtual kern_return_t resetBus(BusResetMode mode = BusResetMode::Normal) = 0;

  // ---- Transaction Primitives (MVP Focus) ----

  // Synchronous quadlet read - primary method for Config ROM access
  // All parameters validated, generation checked for stale transactions
  virtual kern_return_t readQuad(uint16_t nodeID, uint16_t addrHi,
                                 uint32_t addrLo, uint32_t &outValue,
                                 uint32_t atGeneration,
                                 Speed speed = Speed::S400) = 0;

  // ---- Work Queue Integration ----

  // Post work to Link's queue (for controller→hardware calls)
  // Ensures proper thread serialization without exposing DriverKit queues
  virtual void postToLink(std::function<void()> work) = 0;

  // ---- Debugging ----

  // Human-readable identifier for logging
  virtual std::string name() const = 0;

  // ---- Future Extensions (commented for now) ----
  // These will be added as the implementation matures beyond MVP

  // virtual kern_return_t readBlock(uint16_t nodeID,
  //                                 uint16_t addrHi, uint32_t addrLo,
  //                                 void* buffer, size_t length,
  //                                 uint32_t atGeneration, Speed speed) = 0;
  //
  // virtual kern_return_t writeQuad(uint16_t nodeID,
  //                                 uint16_t addrHi, uint32_t addrLo,
  //                                 uint32_t value,
  //                                 uint32_t atGeneration, Speed speed) = 0;
  //
  // virtual kern_return_t allocateIsoChannel(uint32_t channel,
  //                                           Speed speed,
  //                                           uint32_t maxPayload) = 0;
};

// =============================================================================
// ILinkSink Interface - Controller event callbacks
// =============================================================================

// Events delivered from Link (hardware) to Controller (bus orchestration)
// Link guarantees all callbacks happen on Controller's work queue - never on
// ISR
struct ILinkSink {
  using Ptr = std::shared_ptr<ILinkSink>;
  using WeakPtr = std::weak_ptr<ILinkSink>;

  virtual ~ILinkSink() = default;

  // ---- Core Bus Events ----

  // Bus reset detected - new generation started
  virtual void onBusReset(uint32_t generation) = 0;

  // Self-ID phase complete - topology data available
  virtual void onSelfIDs(const SelfIDs &ids) = 0;

  // ---- Optional Events (default implementations provided) ----

  // Isochronous channel allocation changed
  virtual void onIsoMasks(const IsoMask &mask) {}

  // Cycle timer inconsistency detected
  virtual void onCycleInconsistent(uint32_t cycleTime) {}

  // Posted write error occurred
  virtual void onPostedWriteError() {}

  // Unrecoverable bus error - controller should stop operations
  virtual void onBusError(uint32_t errorFlags) {}
};

} // namespace fw