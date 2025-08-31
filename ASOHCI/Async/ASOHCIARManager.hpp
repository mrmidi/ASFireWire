#pragma once
//
// ASOHCIARManager.hpp
// Owns AR Request + Response contexts/rings and surfaces callbacks
//
// Spec refs: OHCI 1.1 §8.1 (programs), §8.2 (regs), §8.4 (buffer-fill), §8.6
// (interrupts)

#include <DriverKit/IOReturn.h>
#include <DriverKit/OSSharedPtr.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <memory>

#include "../Shared/ASOHCITypes.hpp"
#include "ASOHCIARContext.hpp"
#include "ASOHCIARDescriptorRing.hpp"
#include "ASOHCIARParser.hpp"
#include "ASOHCIARStatus.hpp"
#include "ASOHCIARTypes.hpp"

class ASOHCIARManager {
public:
  ASOHCIARManager() = default;
  ~ASOHCIARManager() = default;

  // Create both AR contexts + rings
  kern_return_t Initialize(OSSharedPtr<IOPCIDevice> pci, uint8_t barIndex,
                           uint32_t bufferCount, uint32_t bufferBytes,
                           ARBufferFillMode fillMode,
                           const ARFilterOptions &filterOpts);

  kern_return_t Start();
  kern_return_t Stop();

  // Simple callback style (DriverKit-friendly C function)
  struct ARParsedPacket; // fwd to avoid header include churn
  using PacketCallback = void (*)(void *refcon, const ARParsedPacket &pkt);
  void SetPacketCallback(PacketCallback cb, void *refcon);

  // ISR fan-in from router
  void OnRequestPacketIRQ();
  void OnResponsePacketIRQ();
  void OnRequestBufferIRQ();
  void OnResponseBufferIRQ();

  // Optional: expose pull model too
  bool DequeueRequest(ARPacketView *outView, uint32_t *outIndex);
  bool DequeueResponse(ARPacketView *outView, uint32_t *outIndex);
  kern_return_t RecycleRequest(uint32_t index);
  kern_return_t RecycleResponse(uint32_t index);

private:
  OSSharedPtr<IOPCIDevice> _pci;
  uint8_t _bar = 0;
  std::unique_ptr<ASOHCIARContext> _arReq;
  std::unique_ptr<ASOHCIARContext> _arRsp;
  std::unique_ptr<ASOHCIARDescriptorRing> _ringReq;
  std::unique_ptr<ASOHCIARDescriptorRing> _ringRsp;
  std::unique_ptr<ASOHCIARParser> _parser;
  std::unique_ptr<ASOHCIARStatus> _status;

  PacketCallback _cb = nullptr;
  void *_cbRefcon = nullptr;
};
