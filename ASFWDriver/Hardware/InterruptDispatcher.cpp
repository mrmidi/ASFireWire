#include "InterruptDispatcher.hpp"

#include "../Async/Interfaces/IAsyncSubsystemPort.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Diagnostics/StatusPublisher.hpp"
#include "../Isoch/IsochService.hpp"
#include "../Logging/Logging.hpp"
#include "HardwareInterface.hpp"
#include "OHCIConstants.hpp"
#include "RegisterMap.hpp"

namespace ASFW::Driver {

void InterruptDispatcher::HandleSnapshot(const InterruptSnapshot& snap, ControllerCore& controller,
                                         HardwareInterface& hardware, IODispatchQueue& workQueue,
                                         IsochService& isoch, StatusPublisher& statusPublisher,
                                         ASFW::Async::IAsyncSubsystemPort* asyncSubsystem) {
    controller.HandleInterrupt(snap);

    // ===== ISOCHRONOUS RECEIVE INTERRUPT =====
    // Per OHCI §9.1: kIsochRx (bit 7) indicates one or more IR contexts have completed descriptors.
    // We read isoRecvEvent to determine which contexts, clear it, then dispatch processing.
    if ((snap.intEvent & IntEventBits::kIsochRx) && snap.isoRecvEvent != 0) {
        // Clear the per-context event bits to acknowledge
        hardware.Write(Register32::kIsoRecvIntEventClear, snap.isoRecvEvent);

        // One OHCI IR context backs each capture stream (contextIndex ==
        // streamIndex). A multi-stream DICE device (Venice F32 = 2×16) runs a
        // master (context 0) plus secondary contexts whose event bits are
        // (1 << contextIndex). Drain every signalled context, not just context 0;
        // the secondary's ring would otherwise fill without ever being polled and
        // its channel slice (e.g. 17–32) would never reach the input buffer.
        // Poll the master first so the producer timeline is published before the
        // secondary slices anchor to it.
        const uint32_t recvEvent = snap.isoRecvEvent;
        workQueue.DispatchAsync(^{
          for (uint32_t ctxIdx = 0; ctxIdx < IsochService::kMaxStreamsPerDirection; ++ctxIdx) {
              if ((recvEvent & (1u << ctxIdx)) == 0) {
                  continue;
              }
              if (auto* rx = isoch.ReceiveContext(ctxIdx)) {
                  rx->Poll();
              }
          }
        });
    }

    // ===== ISOCHRONOUS TRANSMIT INTERRUPT =====
    // Per OHCI §9.2: kIsochTx (bit 6) indicates IT context completion.
    // Similar to IR, we read IsoXmitEvent, clear it, and process.
    if ((snap.intEvent & IntEventBits::kIsochTx) && snap.isoXmitEvent != 0) {
        // DEBUG: Sample interrupt rate
        static uint32_t txIrqCtr = 0;
        if ((++txIrqCtr % 100) == 0) {
            ASFW_LOG_V3(Controller, "[IRQ] IsoTx Fired! Count=%u IsoTxEvent=0x%08x", txIrqCtr,
                        snap.isoXmitEvent);
        }

        // Clear event bits to acknowledge
        hardware.Write(Register32::kIsoXmitIntEventClear, snap.isoXmitEvent);

        // Context 0 is our single IT context
        if ((snap.isoXmitEvent & 0x01) && isoch.TransmitContext()) {
            // Process IT directly in ISR context for lowest latency.
            // IT RefillRing is fast (atomic assemble + mem writes).
            // DispatchAsync adds latency which might cause underruns if buffers are small.
            isoch.TransmitContext()->HandleInterrupt();
        }
    }

    if (snap.intEvent != 0) {
        const uint32_t asyncMask = IntEventBits::kReqTxComplete | IntEventBits::kRespTxComplete |
                                   IntEventBits::kARRQ | IntEventBits::kARRS |
                                   IntEventBits::kRQPkt | IntEventBits::kRSPkt;
        if (snap.intEvent & asyncMask) {
            statusPublisher.SetLastAsyncCompletion(mach_absolute_time());
        }

        SharedStatusReason reason = SharedStatusReason::Interrupt;
        if (snap.intEvent & IntEventBits::kBusReset) {
            reason = SharedStatusReason::BusReset;
        } else if (snap.intEvent & asyncMask) {
            reason = SharedStatusReason::AsyncActivity;
        } else if (snap.intEvent & IntEventBits::kUnrecoverableError) {
            reason = SharedStatusReason::Interrupt;
        }

        statusPublisher.Publish(&controller, asyncSubsystem, reason, snap.intEvent);
    }
}

} // namespace ASFW::Driver
