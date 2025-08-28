//
// SelfIDManager.hpp
// Owns Self-ID DMA buffer, OHCI programming, and decode/notify
//
// Responsibilities:
//   • Allocate/map/DMA the self-ID receive buffer (32-bit IOVA)
//   • Program kOHCI_SelfIDBuffer, arm/disarm reception
//   • On Self-ID Complete IRQ: snapshot count, check generation, decode
//   • Emit decoded result to a client callback (e.g., Topology builder)
//
// Notes:
//   • Implementation performs all OHCI register I/O; header stays DK-light.

#pragma once

#include <stdint.h>
#include <functional>
#include <DriverKit/IOLib.h> // kern_return_t

// Global forward declarations for DriverKit classes (pointer use only)
class IOPCIDevice;
class IOBufferMemoryDescriptor;
class IOMemoryMap;
class IODMACommand;
struct IOAddressSegment;

class Topology; // fwd
namespace SelfID { struct Result; }

// Use global DriverKit types directly (defined in :: scope)

class SelfIDManager {
public:
  using DecodeCallback = std::function<void(const SelfID::Result&)>; // every decode
  using StableCallback = std::function<void(const SelfID::Result&)>; // when gen stable

  SelfIDManager() = default;
  ~SelfIDManager() = default;

  SelfIDManager(const SelfIDManager&) = delete;
  SelfIDManager& operator=(const SelfIDManager&) = delete;

  // Initialize DMA resources and program the controller with buffer address.
  // bufferBytes: size to allocate for self-ID buffer (caller chooses policy).
  kern_return_t Initialize(::IOPCIDevice* pci, uint8_t barIndex, uint32_t bufferBytes);

  // Free mappings/DMA and scrub controller programming.
  void Teardown();

  // Arm reception; when clearCount is true, zeros the HW counter before arming.
  kern_return_t Arm(bool clearCount);

  // Handle “Self-ID Complete” in IRQ path.
  // Pass the current value of the SelfIDCount register (latched by caller).
  // Implementation will read the mapped buffer, decode, and fire callbacks.
  void OnSelfIDComplete(uint32_t selfIDCountRegValue);

  // Optional callbacks.
  void SetCallbacks(DecodeCallback onDecode, StableCallback onStable) {
    _onDecode = std::move(onDecode);
    _onStable = std::move(onStable);
  }

  // Accessors (debug/telemetry).
  bool     IsArmed() const { return _armed; }
  bool     InProgress() const { return _inProgress; }
  uint32_t LastGeneration() const { return _lastGeneration; }
  uint64_t BufferIOVA() const;      // returns 0 if uninitialized
  uint32_t BufferBytes() const { return _bufBytes; }

private:
  void programSelfIDBuffer();
  void verifyGenerationAndDispatch(uint32_t countRegValue);

private:
  // Owned resources
  ::IOPCIDevice*             _pci = nullptr;
  uint8_t                    _bar = 0;

  ::IOBufferMemoryDescriptor*  _buf = nullptr;
  ::IOMemoryMap*               _map = nullptr;
  ::IODMACommand*              _dma = nullptr;
  ::IOAddressSegment*          _seg = nullptr; // points to first segment (impl allocates)

  uint32_t                   _bufBytes = 0;

  // State
  bool                       _armed = false;
  bool                       _inProgress = false;
  uint32_t                   _lastGeneration = 0;

  // Callbacks
  DecodeCallback             _onDecode;
  StableCallback             _onStable;
};
