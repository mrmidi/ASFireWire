#include "ControllerCore.hpp"

#include <DriverKit/IOLib.h>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "../Async/DMAMemoryImpl.hpp"
#include "../Async/FireWireBusImpl.hpp"
#include "../Async/Interfaces/IAsyncControllerPort.hpp"
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
    LogInterruptContext(snapshot, rawEvents, currentMask, events);
    HandleFaultInterrupts(events);
    NotifyBusResetCoordinator(events, snapshot.timestamp);
    DispatchAsyncInterrupts(events);
    LogBusResetCompletionEvents(events, snapshot.timestamp);

    const uint32_t faultAcks = FaultAckMask(events);

    if (faultAcks != 0U) {
        hw.ClearIntEvents(faultAcks);
    }

    // Only clear non-reset, non-sticky completion events generically here.
    uint32_t toAck = events & ~(IntEventBits::kBusReset | IntEventBits::kSelfIDComplete |
                                IntEventBits::kSelfIDComplete2 | faultAcks);
    if (toAck != 0U) {
        hw.ClearIntEvents(toAck);
    }
    hw.ClearIsoXmitEvents(snapshot.isoXmitEvent);
    hw.ClearIsoRecvEvents(snapshot.isoRecvEvent);
}

void ControllerCore::LogInterruptContext(const InterruptSnapshot& snapshot,
                                         uint32_t rawEvents,
                                         uint32_t currentMask,
                                         uint32_t events) const {
    if (rawEvents != events) {
        ASFW_LOG_V3(Controller, "Filtered masked interrupts: raw=0x%08x enabled=0x%08x mask=0x%08x",
                    rawEvents, events, currentMask);
    }

    if (deps_.busReset && deps_.busReset->GetState() != BusResetCoordinator::State::Idle) {
        ASFW_LOG_V2(
            Controller,
            "🔍 BUS RESET ACTIVE - Raw interrupt: 0x%08x @ %llu ns (mask=0x%08x filtered=0x%08x)",
            rawEvents, snapshot.timestamp, currentMask, events);
    }

    ASFW_LOG_V3(Controller, "HandleInterrupt: events=0x%08x AsyncSubsystem=%p", events,
                deps_.asyncController.get());

    const std::string eventDecode = DiagnosticLogger::DecodeInterruptEvents(events);
    ASFW_LOG_V3(Controller, "%{public}s", eventDecode.c_str());
}

void ControllerCore::HandleFaultInterrupts(uint32_t events) {
    if ((events & IntEventBits::kUnrecoverableError) != 0U) {
        ASFW_LOG_V0(Controller,
                    "❌ CRITICAL: UnrecoverableError interrupt - hardware fault detected!");
        DiagnoseUnrecoverableError();
    }

    if ((events & IntEventBits::kRegAccessFail) != 0U) {
        ASFW_LOG_V0(Controller, "❌ CRITICAL: regAccessFail - CSR register access failed!");
        ASFW_LOG_V0(Controller,
                    "This indicates hardware could not complete a register read/write operation");
        ASFW_LOG_V0(
            Controller,
            "Common causes: Self-ID buffer access, Config ROM mapping, or context register access");
    }

    if ((events & IntEventBits::kCycleTooLong) != 0U) {
        ASFW_LOG(Controller, "⚠️ WARNING: Cycle too long - isochronous cycle overran 125μs budget");
        ASFW_LOG(Controller,
                 "This indicates DMA descriptors or system latency causing timing violation");
    }

    if ((events & IntEventBits::kPostedWriteErr) != 0U) {
        ASFW_LOG(Controller,
                 "❌ CRITICAL: Posted write error - DMA posted write to host memory failed!");
        ASFW_LOG(Controller, "This indicates IOMMU mapping error or invalid DMA target address");
        ASFW_LOG(Controller, "Common causes: Self-ID buffer DMA, Config ROM shadow update");
    }

    if ((events & IntEventBits::kCycle64Seconds) != 0U) {
        HandleCycle64Seconds();
    }
}

void ControllerCore::NotifyBusResetCoordinator(uint32_t events, uint64_t timestamp) const {
    const uint32_t busResetRelevantBits =
        IntEventBits::kBusReset | IntEventBits::kSelfIDComplete | IntEventBits::kSelfIDComplete2;
    if (deps_.busReset && ((events & busResetRelevantBits) != 0U)) {
        deps_.busReset->OnIrq(events & busResetRelevantBits, timestamp);
    }
}

void ControllerCore::DispatchAsyncInterrupts(uint32_t events) const {
    if (!deps_.asyncController) {
        return;
    }

    if ((events & IntEventBits::kReqTxComplete) != 0U) {
        ASFW_LOG_V3(Controller, "AT Request complete interrupt (transmit done)");
        deps_.asyncController->OnTxInterrupt();
    }

    if ((events & IntEventBits::kRespTxComplete) != 0U) {
        ASFW_LOG_V3(Controller, "AT Response complete interrupt (transmit done)");
        deps_.asyncController->OnTxInterrupt();
    }

    if ((events & (IntEventBits::kARRQ | IntEventBits::kRQPkt)) != 0U) {
        ASFW_LOG_V3(Controller,
                    "AR Request interrupt (ARRQ/RQPkt: async request DMA/packet available)");
        deps_.asyncController->OnRxRequestInterrupt();
    }

    if ((events & (IntEventBits::kARRS | IntEventBits::kRSPkt)) != 0U) {
        ASFW_LOG_V3(Controller,
                    "AR Response interrupt (ARRS/RSPkt: async response DMA/packet available)");
        deps_.asyncController->OnRxResponseInterrupt();
    }
}

void ControllerCore::LogBusResetCompletionEvents(uint32_t events, uint64_t timestamp) const {
    if ((events & IntEventBits::kBusReset) != 0U) {
        ASFW_LOG(Controller, "Bus reset detected @ %llu ns", timestamp);
    }
    if ((events & IntEventBits::kSelfIDComplete) != 0U) {
        ASFW_LOG(Hardware, "Self-ID Complete (bit16)");
    }
    if ((events & IntEventBits::kSelfIDComplete2) != 0U) {
        ASFW_LOG(Hardware, "Self-ID Complete2 (bit15, sticky)");
    }
}

uint32_t ControllerCore::FaultAckMask(uint32_t events) noexcept {
    return events & (IntEventBits::kPostedWriteErr |
                     IntEventBits::kUnrecoverableError |
                     IntEventBits::kRegAccessFail);
}

} // namespace ASFW::Driver
