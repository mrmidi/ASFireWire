#include "ControllerCore.hpp"

#include <DriverKit/IOLib.h>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "../Async/AsyncSubsystem.hpp"
#include "../Async/DMAMemoryImpl.hpp"
#include "../Async/FireWireBusImpl.hpp"
#include "../Bus/BusResetCoordinator.hpp"
#include "../Bus/SelfIDCapture.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../ConfigROM/ConfigROMBuilder.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ConfigROMStore.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Diagnostics/DiagnosticLogger.hpp"
#include "../Diagnostics/MetricsSink.hpp"
#include "../Discovery/DeviceManager.hpp"
#include "../Discovery/DeviceRegistry.hpp"
#include "../Discovery/SpeedPolicy.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Hardware/IEEE1394.hpp"
#include "../Hardware/InterruptManager.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "../Hardware/OHCIEventCodes.hpp"
#include "../Hardware/RegisterMap.hpp"
#include "../IRM/IRMClient.hpp"
#include "../Protocols/AVC/AVCDiscovery.hpp"
#include "../Protocols/AVC/CMP/CMPClient.hpp"
#include "../Protocols/Audio/DeviceProtocolFactory.hpp"
#include "../Scheduling/Scheduler.hpp"
#include "../Version/DriverVersion.hpp"
#include "ControllerStateMachine.hpp"
#include "Logging.hpp"

namespace ASFW::Driver {

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ControllerCore::HandleInterrupt(const InterruptSnapshot& snapshot) {
    if (!running_ || !deps_.hardware) {
        ASFW_LOG(Controller, "HandleInterrupt early return (running=%d hw=%p)", running_,
                 deps_.hardware.get());
        return;
    }

    auto& hw = *deps_.hardware;
    const uint32_t rawEvents = snapshot.intEvent;

    // OHCI §5.7: IntMaskSet/IntMaskClear are write-only strobes - reading returns undefined value
    const uint32_t currentMask = deps_.interrupts ? deps_.interrupts->EnabledMask() : 0xFFFFFFFF;
    const uint32_t events = rawEvents & currentMask;

    if (rawEvents != events) {
        ASFW_LOG_V3(Controller, "Filtered masked interrupts: raw=0x%08x enabled=0x%08x mask=0x%08x",
                    rawEvents, events, currentMask);
    }

    // RAW INTERRUPT LOGGING: Log every interrupt during bus reset for diagnostics
    // This helps diagnose timing issues, missing interrupts, and hardware quirks
    if (deps_.busReset && deps_.busReset->GetState() != BusResetCoordinator::State::Idle) {
        ASFW_LOG_V2(
            Controller,
            "🔍 BUS RESET ACTIVE - Raw interrupt: 0x%08x @ %llu ns (mask=0x%08x filtered=0x%08x)",
            rawEvents, snapshot.timestamp, currentMask, events);
    }

    ASFW_LOG_V3(Controller, "HandleInterrupt: events=0x%08x AsyncSubsystem=%p", events,
                deps_.asyncSubsystem.get());

    // Detailed interrupt decode (adapted from Linux log_irqs)
    const std::string eventDecode = DiagnosticLogger::DecodeInterruptEvents(events);
    ASFW_LOG_V3(Controller, "%{public}s", eventDecode.c_str());

    // Check for critical hardware errors first
    if ((events & IntEventBits::kUnrecoverableError) != 0U) {
        ASFW_LOG_V0(Controller,
                    "❌ CRITICAL: UnrecoverableError interrupt - hardware fault detected!");
        DiagnoseUnrecoverableError();
        // TODO(ASFW-Error): Implement error recovery or halt driver
    }

    // Check for CSR register access failures (often occurs with UnrecoverableError)
    if ((events & IntEventBits::kRegAccessFail) != 0U) {
        ASFW_LOG_V0(Controller, "❌ CRITICAL: regAccessFail - CSR register access failed!");
        ASFW_LOG_V0(Controller,
                    "This indicates hardware could not complete a register read/write operation");
        ASFW_LOG_V0(
            Controller,
            "Common causes: Self-ID buffer access, Config ROM mapping, or context register access");
    }

    // Check for cycle timing errors (adapted from Linux irq handler)
    if ((events & IntEventBits::kCycleTooLong) != 0U) {
        ASFW_LOG(Controller, "⚠️ WARNING: Cycle too long - isochronous cycle overran 125μs budget");
        ASFW_LOG(Controller,
                 "This indicates DMA descriptors or system latency causing timing violation");
        // Per OHCI §6.2.1: cycleTooLong fires when cycle exceeds 125μs nominal
    }

    // Per Linux irq_handler: postedWriteErr very often pairs with unrecoverableError
    // Most common cause: Self-ID buffer or Config ROM DMA address invalid/unmapped
    // OHCI §13.2.4: Hardware detected error during posted write DMA cycle to host memory
    if ((events & IntEventBits::kPostedWriteErr) != 0U) {
        ASFW_LOG(Controller,
                 "❌ CRITICAL: Posted write error - DMA posted write to host memory failed!");
        ASFW_LOG(Controller, "This indicates IOMMU mapping error or invalid DMA target address");
        ASFW_LOG(Controller, "Common causes: Self-ID buffer DMA, Config ROM shadow update");
        // Per OHCI §13.2.4: Hardware detected error during posted write DMA cycle
    }

    if ((events & IntEventBits::kCycle64Seconds) != 0U) {
        HandleCycle64Seconds();
    }

    // Feed relevant events to BusResetCoordinator FSM (it filters what it needs)
    const uint32_t busResetRelevantBits =
        IntEventBits::kBusReset | IntEventBits::kSelfIDComplete | IntEventBits::kSelfIDComplete2 |
        IntEventBits::kUnrecoverableError | IntEventBits::kRegAccessFail;
    if (deps_.busReset && ((events & busResetRelevantBits) != 0U)) {
        deps_.busReset->OnIrq(events & busResetRelevantBits, snapshot.timestamp);
    }

    // Dispatch AT Request completions
    if (deps_.asyncSubsystem && ((events & IntEventBits::kReqTxComplete) != 0U)) {
        ASFW_LOG_V3(Controller, "AT Request complete interrupt (transmit done)");
        deps_.asyncSubsystem->OnTxInterrupt();
        // TODO(ASFW-Logging): Add DiagnosticLogger::DecodeAsyncPacket() call once we extract packet
        // headers
    }

    // Dispatch AT Response completions
    if (deps_.asyncSubsystem && ((events & IntEventBits::kRespTxComplete) != 0U)) {
        ASFW_LOG_V3(Controller, "AT Response complete interrupt (transmit done)");
        deps_.asyncSubsystem->OnTxInterrupt();
    }

    if (deps_.asyncSubsystem && ((events & IntEventBits::kRQPkt) != 0U)) {
        ASFW_LOG_V3(Controller, "AR Request interrupt (RQPkt: async receive packet available)");
        deps_.asyncSubsystem->OnRxInterrupt(ASFW::Async::AsyncSubsystem::ARContextType::Request);
    }

    if (deps_.asyncSubsystem && ((events & IntEventBits::kRSPkt) != 0U)) {
        ASFW_LOG_V3(Controller, "AR Response interrupt (RSPkt: async receive packet available)");
        deps_.asyncSubsystem->OnRxInterrupt(ASFW::Async::AsyncSubsystem::ARContextType::Response);
    }

    if ((events & IntEventBits::kBusReset) != 0U) {
        ASFW_LOG(Controller, "Bus reset detected @ %llu ns", snapshot.timestamp);

        if (deps_.interrupts) {
            deps_.interrupts->MaskInterrupts(&hw, IntEventBits::kBusReset);
        }
    }

    if ((events & IntEventBits::kSelfIDComplete) != 0U) { // 0x0001_0000 bit 16
        ASFW_LOG(Hardware, "Self-ID Complete (bit16)");
    }

    if ((events & IntEventBits::kSelfIDComplete2) != 0U) { // 0x0000_8000 bit 15
        ASFW_LOG(Hardware, "Self-ID Complete2 (bit15, sticky)");
    }

    // FSM handles all of the above through proper state machine transitions

    // Only clear non-reset events here (AR/AT completions, errors, etc.)
    uint32_t toAck = events & ~(IntEventBits::kBusReset | IntEventBits::kSelfIDComplete |
                                IntEventBits::kSelfIDComplete2);
    if (toAck != 0U) {
        hw.ClearIntEvents(toAck);
    }
    hw.ClearIsoXmitEvents(snapshot.isoXmitEvent);
    hw.ClearIsoRecvEvents(snapshot.isoRecvEvent);
}

} // namespace ASFW::Driver

