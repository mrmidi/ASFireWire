// ASOHCIInterruptDump.cpp
#include "ASOHCIInterruptDump.hpp"
#include "LogHelper.hpp"
#include "OHCIConstants.hpp"
#include <os/log.h>

namespace LogUtils {

void DumpIntEvent(uint32_t ev) {
  if (!ev)
    return;

  uint32_t dmaEvents = ev & 0x000000FF;
  if (dmaEvents) {
    os_log(ASLog(), "ASOHCI: === DMA Completion Interrupts (OHCI §6.1) ===");
    if (ev & kOHCI_Int_ReqTxComplete)
      os_log(ASLog(), "ASOHCI:  • AT Request Tx Complete (bit 0) - §7.6");
    if (ev & kOHCI_Int_RespTxComplete)
      os_log(ASLog(), "ASOHCI:  • AT Response Tx Complete (bit 1) - §7.6");
    if (ev & kOHCI_Int_ARRQ)
      os_log(ASLog(), "ASOHCI:  • AR Request DMA Complete (bit 2) - §8.6");
    if (ev & kOHCI_Int_ARRS)
      os_log(ASLog(), "ASOHCI:  • AR Response DMA Complete (bit 3) - §8.6");
    if (ev & kOHCI_Int_RqPkt)
      os_log(ASLog(), "ASOHCI:  • AR Request Packet Received (bit 4) - §8.6");
    if (ev & kOHCI_Int_RsPkt)
      os_log(ASLog(), "ASOHCI:  • AR Response Packet Received (bit 5) - §8.6");
    if (ev & kOHCI_Int_IsochTx)
      os_log(ASLog(), "ASOHCI:  • Isochronous Tx Interrupt (bit 6) - §6.3");
    if (ev & kOHCI_Int_IsochRx)
      os_log(ASLog(), "ASOHCI:  • Isochronous Rx Interrupt (bit 7) - §6.4");
  }

  uint32_t errors =
      ev & (kOHCI_Int_PostedWriteErr | kOHCI_Int_LockRespErr |
            kOHCI_Int_RegAccessFail | kOHCI_Int_UnrecoverableError |
            kOHCI_Int_CycleTooLong);
  if (errors) {
    os_log(ASLog(), "ASOHCI: === ERROR CONDITIONS (OHCI §6.1) ===");
    if (ev & kOHCI_Int_PostedWriteErr)
      os_log(
          ASLog(),
          "ASOHCI:  • Posted Write Error (bit 8) - host bus error §13.2.8.1");
    if (ev & kOHCI_Int_LockRespErr)
      os_log(ASLog(),
             "ASOHCI:  • Lock Response Error (bit 9) - no ack_complete §5.5.1");
    if (ev & kOHCI_Int_RegAccessFail)
      os_log(ASLog(),
             "ASOHCI:  • Register Access Failed (bit 18) - missing SCLK clock");
    if (ev & kOHCI_Int_UnrecoverableError)
      os_log(ASLog(), "ASOHCI:  • UNRECOVERABLE ERROR (bit 24) - context dead, "
                      "operations stopped");
    if (ev & kOHCI_Int_CycleTooLong)
      os_log(ASLog(), "ASOHCI:  • Cycle Too Long (bit 25) - >120μs cycle, "
                      "cycleMaster cleared");
  }

  uint32_t busEvents =
      ev & (kOHCI_Int_SelfIDComplete2 | kOHCI_Int_SelfIDComplete |
            kOHCI_Int_BusReset | kOHCI_Int_Phy | kOHCI_Int_CycleSynch |
            kOHCI_Int_Cycle64Seconds | kOHCI_Int_CycleLost |
            kOHCI_Int_CycleInconsistent);
  if (busEvents) {
    os_log(ASLog(), "ASOHCI: === Bus Management & Timing (OHCI §6.1) ===");
    if (ev & kOHCI_Int_BusReset)
      os_log(ASLog(),
             "ASOHCI:  • Bus Reset (bit 17) - PHY entered reset mode §6.1.1");
    if (ev & kOHCI_Int_SelfIDComplete)
      os_log(ASLog(), "ASOHCI:  • Self-ID Complete (bit 16) - packet stream "
                      "received §11.5");
    if (ev & kOHCI_Int_SelfIDComplete2)
      os_log(ASLog(), "ASOHCI:  • Self-ID Complete Secondary (bit 15) - "
                      "independent of busReset §11.5");
    if (ev & kOHCI_Int_Phy)
      os_log(ASLog(),
             "ASOHCI:  • PHY Interrupt (bit 19) - status transfer request");
    if (ev & kOHCI_Int_CycleSynch)
      os_log(ASLog(),
             "ASOHCI:  • Cycle Start (bit 20) - new isochronous cycle begun");
    if (ev & kOHCI_Int_Cycle64Seconds)
      os_log(ASLog(), "ASOHCI:  • 64 Second Tick (bit 21) - cycle second "
                      "counter bit 7 changed");
    if (ev & kOHCI_Int_CycleLost)
      os_log(ASLog(), "ASOHCI:  • Cycle Lost (bit 22) - no cycle_start between "
                      "cycleSynch events");
    if (ev & kOHCI_Int_CycleInconsistent)
      os_log(ASLog(), "ASOHCI:  • Cycle Inconsistent (bit 23) - timer mismatch "
                      "§5.13, §9.5.1, §10.5.1");
  }

  uint32_t highBits = ev & 0xFC000000;
  if (highBits) {
    os_log(ASLog(), "ASOHCI: === High-Order Interrupts (OHCI §6.1) ===");
    if (ev & kOHCI_Int_PhyRegRcvd)
      os_log(ASLog(),
             "ASOHCI:  • PHY Register Received (bit 26) - PHY register packet");
    if (ev & kOHCI_Int_AckTardy)
      os_log(ASLog(),
             "ASOHCI:  • Acknowledgment Tardy (bit 27) - late ack received");
    if (ev & kOHCI_Int_SoftInterrupt)
      os_log(ASLog(),
             "ASOHCI:  • Software Interrupt (bit 28) - host-initiated");
    if (ev & kOHCI_Int_VendorSpecific)
      os_log(ASLog(),
             "ASOHCI:  • Vendor Specific (bit 29) - implementation-defined");
    if (ev & kOHCI_Int_MasterEnable)
      os_log(ASLog(),
             "ASOHCI:  • Master Interrupt Enable (bit 31) - global enable bit");
  }

  uint32_t reserved = ev & 0x00007C00; // Bits 10-14 are reserved per OHCI spec
  if (reserved) {
    os_log(ASLog(),
           "ASOHCI: WARNING: Reserved interrupt bits set: 0x%08x (bits 10-14)",
           reserved);
  }

  uint32_t bit30 = ev & 0x40000000; // Bit 30 is reserved per OHCI spec
  if (bit30) {
    os_log(ASLog(), "ASOHCI: WARNING: Reserved interrupt bit 30 set: 0x%08x",
           bit30);
  }
}

} // namespace LogUtils
