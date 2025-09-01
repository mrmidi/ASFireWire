#pragma once
//
// ASOHCIInterruptRouter.hpp
// Thin, shared interrupt fan-out for AT & AR contexts.
//
// Spec refs: OHCI 1.1 §3.1 (context events), AT §7.6 (AT interrupts),
//            AR §8.6 (AR interrupts)
//
// This class doesn’t touch HW directly; it’s fed from your top-level
// controller ISR/filter and calls into registered contexts.
//

#include <DriverKit/IOLib.h>
#include <DriverKit/OSObject.h>
#include <memory>

// Forward declarations for manager classes
class ASOHCIATManager;
class ASOHCIARManager;
class ASOHCIITManager;
class ASOHCIIRManager;
class ASOHCI;

// Forward declaration for new RAII architecture
namespace fw {
class LinkHandle;
}

// Type alias for weak_ptr to avoid std:: qualification issues in DriverKit
using LinkHandleWeakPtr = std::weak_ptr<fw::LinkHandle>;

class ASOHCIInterruptRouter : public OSObject {

public:
  // Register managers (any may be nullptr)
  virtual void SetATManager(ASOHCIATManager *m);
  virtual void SetARManager(ASOHCIARManager *m);
  virtual void SetITManager(ASOHCIITManager *m);
  virtual void SetIRManager(ASOHCIIRManager *m);
  virtual void SetController(ASOHCI *ohci);

  // RAII Architecture Integration
  virtual void SetLinkHandle(LinkHandleWeakPtr linkHandle);

  // Dispatchers called from the controller’s ISR path
  // AT Tx path (§7.6)
  virtual void OnAT_Request_TxComplete();
  virtual void OnAT_Response_TxComplete();

  // AR Rx path (§8.6)
  virtual void OnAR_Request_PacketArrived();
  virtual void OnAR_Response_PacketArrived();

  // Isochronous masks (call with mask read from controller registers)
  virtual void OnIsoTxMask(uint32_t mask);
  virtual void OnIsoRxMask(uint32_t mask);

  // Host cycle inconsistent (fan-out to IT policy with rate limiting)
  virtual void OnCycleInconsistent(uint64_t time);

  // Bus reset and Self-ID handling (called from ISR top-level)
  virtual void OnBusReset(uint64_t time);
  virtual void OnSelfIDComplete(uint32_t selfIDCountReg, uint32_t generation,
                                bool errorFlag);

  // Other host events
  virtual void OnPostedWriteError();
  virtual void OnCycleTooLong();

public:
  // Static creation method for DriverKit compatibility
  static ASOHCIInterruptRouter *Create() {
    return new (IOMalloc(sizeof(ASOHCIInterruptRouter)))
        ASOHCIInterruptRouter();
  }

private:
  void ProcessSelfIDComplete(uint32_t selfIDCountReg, uint32_t generation,
                             bool errorFlag);

private:
  ASOHCIATManager *_at = nullptr;
  ASOHCIARManager *_ar = nullptr;
  ASOHCIITManager *_it = nullptr;
  ASOHCIIRManager *_ir = nullptr;
  ASOHCI *_ohci = nullptr; // backref for ivars + helpers
  class IODispatchQueue *_selfIDQueue = nullptr;

  // RAII Architecture Components
  LinkHandleWeakPtr _linkHandle; // Weak reference to avoid cycles
};
