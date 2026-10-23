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
#include "../Bus/CSR/SpeedMapService.hpp"
#include "../Bus/BusManager/BusManagerElectionDriver.hpp"
#include "../Bus/IRM/IRMFallbackCoordinator.hpp"
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
#include "../Bus/IRM/IRMClient.hpp"
#include "../Protocols/AVC/AVCDiscovery.hpp"
#include "../Protocols/SBP2/Session/SessionRegistry.hpp"
#include "../Protocols/AVC/CMP/CMPClient.hpp"
#include "../Scheduling/Scheduler.hpp"
#include "../Version/DriverVersion.hpp"
#include "ControllerStateMachine.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Driver {

IsochContextEvents ControllerCore::HandleInterrupt(const InterruptSnapshot& snapshot) {
    const auto state = StateMachine().CurrentState();
    const bool admitted = state == ControllerState::kStarting || state == ControllerState::kRunning;
    if (!admitted || !deps_.hardware) {
        ASFW_LOG(Controller, "HandleInterrupt early return (state=%{public}s hw=%p)",
                 ToString(state).data(),
                 deps_.hardware.get());
        return {};
    }

    auto& hw = *deps_.hardware;
    const uint32_t rawEvents = snapshot.intEvent;

    // OHCI §5.7: IntMaskSet/IntMaskClear are write-only strobes - reading returns undefined value
    const uint32_t currentMask = deps_.interrupts ? deps_.interrupts->EnabledMask() : 0xFFFFFFFF;
    const uint32_t events = rawEvents & currentMask;
    LogInterruptContext(snapshot, rawEvents, currentMask, events);
    HandleFaultInterrupts(events);
    NotifyBusResetCoordinator(events, snapshot.timestamp);
    if ((events & IntEventBits::kBusReset) != 0U) {
        // Node numbers are reassigned by the reset; no node stays admitted to
        // physical DMA until its session re-enables it for the new generation.
        hw.ClearPhysicalRequestFilters();
        auto access = hw.TryBeginAccess();
        if (!access) return {};
        // SelfIDCount.selfIDGeneration already holds the new generation at the
        // busReset edge (OHCI 1.2 Table 11-2); 0x06C is reserved.
        const uint32_t generation =
            (access.Read(Register32::kSelfIDCount) & SelfIDCountBits::kGenerationMask) >>
            SelfIDCountBits::kGenerationShift;
        // A reset edge is a hard liveness boundary: no higher layer may retain
        // the old (generation,node) address while Self-ID and ROM discovery are
        // in flight. This matches the legacy IOFireWireFamily policy of
        // invalidating all node IDs before resuming children
        // (IOFireWireController.cpp:1983-2019). GUID identity is retained and
        // rebound by the ensuing discovery scan.
        // Invalidate every remote route before notifying protocol producers.
        // A later discovery pass supplies a new token for the rebound
        // `(device incarnation, route epoch, generation, node)` mapping.
        if (deps_.deviceRegistry) {
            deps_.deviceRegistry->InvalidateLiveMappingsForBusReset();
        }
        // Observed link speeds are keyed by node ID, and the reset reassigns
        // them. Carrying an entry across would apply one device's proven ceiling
        // to whatever lands on that number next. Discovery re-probes before any
        // protocol transacts, so clearing here costs nothing.
        if (deps_.speedPolicy) {
            deps_.speedPolicy->Reset();
        }
        if (deps_.cmpClient) {
            deps_.cmpClient->InvalidateAllLeasesForBusReset();
        }
        if (deps_.avcDiscovery) {
            deps_.avcDiscovery->OnBusReset(generation);
        }
        if (deps_.deviceManager) {
            deps_.deviceManager->SuspendAllForBusReset();
        }
        if (deps_.busManagerElectionDriver) {
            deps_.busManagerElectionDriver->OnBusReset();
        }
        if (localIrmController_) {
            localIrmController_->OnBusResetStarted(generation);
        }
        if (irmFallback_) {
            irmFallback_->OnBusResetStarted(generation);
        }
        if (cyclePolicy_) {
            cyclePolicy_->OnBusResetStarted(generation);
        }
        if (speedMapService_) {
            speedMapService_->Invalidate(generation);
        }
        if (rootSelection_) {
            rootSelection_->OnBusResetStarted(generation);
        }
        if (gapPolicy_) {
            gapPolicy_->OnBusResetStarted(generation);
        }
        if (powerLinkPolicy_) {
            powerLinkPolicy_->OnBusResetStarted(generation);
        }
        if (deps_.sbp2SessionRegistry) {
            deps_.sbp2SessionRegistry->OnBusReset(static_cast<uint16_t>(generation));
        }
    }
    // Acknowledge before servicing the rings, as Linux does (ohci.c:2223-2235).
    // The MSI is one message per 0->1 edge of IntEvent & IntMask. A packet that
    // lands after the ring scan but before the acknowledge would have its RQPkt
    // cleared here and sit in the ring until an unrelated interrupt (Hasselblad
    // H camera, 2026-10-15: status blocks serviced 10 s late). Acknowledged
    // first, it keeps its bit and ServiceUntilDeasserted's re-read runs another
    // pass. busReset stays set until the reset FSM has quiesced AT (OHCI 1.1
    // §7.2.3.2); the fault bits are acknowledged after their handling below.
    // selfIDComplete/selfIDComplete2 are latched: left set and enabled they
    // keep the output asserted and silence every later MSI.
    const uint32_t faultAcks = FaultAckMask(events);
    const uint32_t toAck = events & ~(IntEventBits::kBusReset | faultAcks);
    // Capture and acknowledge isoch context events in the same flushed access
    // before any async ring service; return the captured masks to the dispatcher.
    const IsochContextEvents isochEvents = hw.AcknowledgeInterruptEvents(toAck, rawEvents);

    DispatchAsyncInterrupts(events);
    if ((events & IntEventBits::kBusReset) != 0U &&
        deps_.busResetStartedCallback) {
        // Abort generation-pinned async work before asking higher-level stream
        // sessions to quiesce; otherwise a synchronous start path could hold
        // its session lock while waiting for an abort callback dispatched here.
        deps_.busResetStartedCallback();
    }
    LogBusResetCompletionEvents(events, snapshot.timestamp);

    if (faultAcks != 0U) {
        hw.ClearIntEvents(faultAcks);
    }

    if ((events & IntEventBits::kCycleTooLong) != 0U) {
        HandleCycleTooLong();
    }
    return isochEvents;
}

// A cycle ran past 115-120 us while this node was root and cycle master, and
// the controller cleared LinkControl.cycleMaster (OHCI 1.2 draft Table 6-1,
// Table 5-17). Until something sets it again the bus has no cycle starts, so
// no isochronous stream runs. The cycle policy owns the decision (FW-9/FW-10);
// it re-applies it. Runs after the acknowledge above: cycleMaster stays zero
// while cycleTooLong is set.
void ControllerCore::HandleCycleTooLong() {
    const bool restored =
        cyclePolicy_ && cyclePolicy_->OnCycleTooLong(currentGeneration_, *this);
    ASFW_LOG(Controller, "[CyclePolicy] cycleTooLong gen=%u cycleMaster %{public}s",
             currentGeneration_, restored ? "re-enabled" : "not ours; left off");
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

    // Keep decoding inside the verbosity gate: hidden IRQ logs must not
    // allocate or format strings on every interrupt.
    if (ASFW_GET_VERBOSITY(Controller) >= 3) {
        const std::string eventDecode = DiagnosticLogger::DecodeInterruptEvents(events);
        ASFW_LOG(Controller, "%{public}s", eventDecode.c_str());
    }
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


    if ((events & IntEventBits::kCycleInconsistent) != 0U) {
        const bool busResetActive =
            deps_.busReset && deps_.busReset->GetState() != BusResetCoordinator::State::Idle;
        const bool resetWindowEvent =
            (events & (IntEventBits::kBusReset |
                       IntEventBits::kSelfIDComplete |
                       IntEventBits::kSelfIDComplete2)) != 0U;

        if (!busTimeRunning_ || busResetActive || resetWindowEvent) {
            ASFW_LOG_V2(
                Controller,
                "Ignoring cycleInconsistent during controller bring-up/reset (busTimeRunning=%d busResetActive=%d resetWindowEvent=%d)",
                busTimeRunning_ ? 1 : 0,
                busResetActive ? 1 : 0,
                resetWindowEvent ? 1 : 0);
        } else {
            ASFW_LOG_WARNING(
                Controller,
                "⚠️ WARNING: cycleInconsistent - cycle timer lost consistency; scheduling duplex recovery");
            if (deps_.cycleInconsistentCallback) {
                deps_.cycleInconsistentCallback();
            }
        }
    }

    if ((events & IntEventBits::kPostedWriteErr) != 0U) {
        if (auto access = deps_.hardware->TryBeginAccess()) {
            // Linux ohci.c:2121-2125 reads the fault address before acknowledging.
            const uint32_t hi = access.Read(Register32::kPostedWriteAddressHi);
            const uint32_t lo = access.Read(Register32::kPostedWriteAddressLo);
            ASFW_LOG(Controller, "Posted write error: addressHi=0x%08x addressLo=0x%08x", hi, lo);
        }
    }

    if ((events & IntEventBits::kCycle64Seconds) != 0U) {
        HandleCycle64Seconds();
    }

    // FW-8: record cycleLost evidence for RoleCoordinator. cycleSynch is
    // deliberately ignored by CycleObserver because it is local timer evidence,
    // not proof that the remote root generated cycle-start packets.
    if (cycleObserver_.OnInterrupt(currentGeneration_, events)) {
        if (((events & IntEventBits::kCycleLost) != 0U) && cycleLostWindowActive_) {
            CompleteRootCycleLostWindow(currentGeneration_, cycleLostWindowEpoch_, true);
        } else {
            roleCoordinator_.OnCycleStartEvidence(currentGeneration_,
                                                  cycleObserver_.Observation());
            EvaluateCyclePolicy();
        }
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
