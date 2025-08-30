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

#include <DriverKit/OSObject.h>

class ASOHCIContextBase;

class ASOHCIInterruptRouter : public OSObject {
  OSDeclareDefaultStructors(ASOHCIInterruptRouter)

      public :
      // Register contexts (any may be nullptr)
      virtual void SetATRequest(ASOHCIContextBase *c);
  virtual void SetATResponse(ASOHCIContextBase *c);
  virtual void SetARRequest(ASOHCIContextBase *c);
  virtual void SetARResponse(ASOHCIContextBase *c);

  // Dispatchers called from the controller’s ISR path
  // AT Tx path (§7.6)
  virtual void OnAT_Request_TxComplete();
  virtual void OnAT_Response_TxComplete();

  // AR Rx path (§8.6) — packet arrivals vs buffer completions
  virtual void OnAR_Request_PacketArrived();
  virtual void OnAR_Response_PacketArrived();
  virtual void OnAR_Request_BufferComplete();
  virtual void OnAR_Response_BufferComplete();

private:
  ASOHCIContextBase *_atReq = nullptr;
  ASOHCIContextBase *_atRsp = nullptr;
  ASOHCIContextBase *_arReq = nullptr;
  ASOHCIContextBase *_arRsp = nullptr;
};
