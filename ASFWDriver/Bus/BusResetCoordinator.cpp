#include "BusResetCoordinator.hpp"

#ifdef ASFW_HOST_TEST
#include <chrono>
#include <thread>
#else
#include <DriverKit/IOLib.h>
#endif

#include "../Async/AsyncSubsystem.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "HardwareInterface.hpp"
#include "InterruptManager.hpp"
#include "Logging.hpp"
#include "SelfIDCapture.hpp"
#include "TopologyManager.hpp"
// NEW: BusManager.hpp
#include "BusManager.hpp"

namespace ASFW::Driver {

BusResetCoordinator::BusResetCoordinator() = default;
BusResetCoordinator::~BusResetCoordinator() = default;

void BusResetCoordinator::Initialize(HardwareInterface* hw, OSSharedPtr<IODispatchQueue> workQueue,
                                     Async::AsyncSubsystem* asyncSys, SelfIDCapture* selfIdCapture,
                                     ConfigROMStager* configRom, InterruptManager* interrupts,
                                     TopologyManager* topology, BusManager* busManager,
                                     Discovery::ROMScanner* romScanner) {
    hardware_ = hw;
    workQueue_ = std::move(workQueue);
    asyncSubsystem_ = asyncSys;
    selfIdCapture_ = selfIdCapture;
    configRomStager_ = configRom;
    interruptManager_ = interrupts;
    topologyManager_ = topology;
    busManager_ = busManager;
    romScanner_ = romScanner;
    pendingPhyCommand_.reset();
    pendingPhyReason_.clear();
    pendingManagedReset_ = false;

    if (hardware_ == nullptr || workQueue_.get() == nullptr || asyncSubsystem_ == nullptr ||
        selfIdCapture_ == nullptr || configRomStager_ == nullptr || interruptManager_ == nullptr ||
        topologyManager_ == nullptr) {
        ASFW_LOG(BusReset, "ERROR: BusResetCoordinator initialized with null dependencies!");
    }

    state_ = State::Idle;
    selfIDComplete1_ = false;
    selfIDComplete2_ = false;
}

// ISR-safe event dispatcher - just posts events to FSM
void BusResetCoordinator::OnIrq(uint32_t intEvent, uint64_t timestamp) {
    bool relevant = false;

    if ((intEvent & IntEventBits::kBusReset) != 0U) {
        relevant = true;
        lastResetNs_ = timestamp;
        ProcessEvent(Event::IrqBusReset);
    }

    if ((intEvent & IntEventBits::kSelfIDComplete) != 0U) {
        relevant = true;
        lastSelfIdNs_ = timestamp;
        ProcessEvent(Event::IrqSelfIDComplete);
    }

    if ((intEvent & IntEventBits::kSelfIDComplete2) != 0U) {
        relevant = true;
        ProcessEvent(Event::IrqSelfIDComplete2);
    }

    if ((intEvent & IntEventBits::kUnrecoverableError) != 0U) {
        relevant = true;
        ProcessEvent(Event::Unrecoverable);
    }

    if ((intEvent & IntEventBits::kRegAccessFail) != 0U) {
        relevant = true;
        ProcessEvent(Event::RegFail);
    }

    // Only schedule FSM if relevant bits were present
    if (relevant && (workQueue_.get() != nullptr)) {
        ASFW_LOG(BusReset, "OnIrq: Scheduling RunStateMachine on workQueue (state=%{public}s)",
                 StateString());
        workQueue_->DispatchAsync(^{
          RunStateMachine();
        });
    }
}

void BusResetCoordinator::BindCallbacks(TopologyReadyCallback onTopology) {
    topologyCallback_ = std::move(onTopology);
}

uint64_t BusResetCoordinator::MonotonicNow() {
#ifdef ASFW_HOST_TEST
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
#else
    static mach_timebase_info_data_t info{};
    if (info.denom == 0) {
        mach_timebase_info(&info);
    }
    const uint64_t ticks = mach_absolute_time();
    return ticks * info.numer / info.denom;
#endif
}

// ============================================================================
// FSM Implementation
// ============================================================================

const char* BusResetCoordinator::StateString() const { return StateString(state_); }

const char* BusResetCoordinator::StateString(State state) {
    switch (state) {
    case State::Idle:
        return "Idle";
    case State::Detecting:
        return "Detecting";
    case State::WaitingSelfID:
        return "WaitingSelfID";
    case State::QuiescingAT:
        return "QuiescingAT";
    case State::RestoringConfigROM:
        return "RestoringConfigROM";
    case State::ClearingBusReset:
        return "ClearingBusReset";
    case State::Rearming:
        return "Rearming";
    case State::Complete:
        return "Complete";
    case State::Error:
        return "Error";
    }
    return "Unknown";
}

// ============================================================================
// FSM Guards
// ============================================================================

bool BusResetCoordinator::G_ATInactive() {
    // Per Linux ohci.c context_stop(): Poll CONTEXT_ACTIVE bit with timeout
    // Linux polls up to 1000 times with 10μs delay (max 10ms total)
    // DriverKit can't block that long, so we do a few quick polls and reschedule if needed

    if (hardware_ == nullptr) {
        return false;
    }

    // OHCI §3.1: ContextControl is read/write; *Set/*Clear are write-only strobes
    // Read from ControlSet offset (same as Base for AT contexts) to get current .active/.run state
    const uint32_t atReqControl =
        hardware_->Read(Register32FromOffsetUnchecked(DMAContextHelpers::AsReqTrContextControlSet));
    const uint32_t atRspControl =
        hardware_->Read(Register32FromOffsetUnchecked(DMAContextHelpers::AsRspTrContextControlSet));

    const bool atReqActive = (atReqControl & kContextControlActiveBit) != 0;
    const bool atRspActive = (atRspControl & kContextControlActiveBit) != 0;

    // OHCI §3.1.1.3 — ContextControl.active:
    // Hardware clears this bit after bus reset when DMA controller reaches safe stop point
    // Per §7.2.3.2: Software must wait for .active==0 before clearing busReset interrupt

    const bool inactive = !atReqActive && !atRspActive;

    ASFW_LOG_BUSRESET_DETAIL("[Guard] AT contexts %{public}s: Req=%d Rsp=%d",
                             inactive ? "INACTIVE (safe)" : "active (retry)", atReqActive,
                             atRspActive);

    return inactive;
}

bool BusResetCoordinator::G_HaveSelfIDPair() const { return selfIDComplete1_ && selfIDComplete2_; }

bool BusResetCoordinator::G_ROMImageReady() {
    // NOTE: Simple null-check validates ConfigROMStager is initialized and ready.
    // ConfigROMStager::StageImage() must be called during ControllerCore::Start()
    // before any bus reset occurs. Non-null pointer indicates successful staging.
    // Future enhancement: Add explicit ConfigROMStager::IsReady() status method.
    return configRomStager_ != nullptr;
}

bool BusResetCoordinator::G_NodeIDValid() const {
    if (hardware_ == nullptr) {
        return false;
    }
    uint32_t nodeId = hardware_->Read(Register32::kNodeID);
    // Check iDValid bit and nodeNumber != 63
    return ((nodeId & 0x80000000U) != 0U) && ((nodeId & 0x3FU) != 63U);
}

} // namespace ASFW::Driver
