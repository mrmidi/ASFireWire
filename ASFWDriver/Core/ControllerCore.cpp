#include "ControllerCore.hpp"

#include <DriverKit/IOLib.h>
#include <cstdio>
#include <string>

#include "../Async/AsyncSubsystem.hpp"
#include "../Async/OHCIEventCodes.hpp"
#include "BusResetCoordinator.hpp"
#include "ConfigROMBuilder.hpp"
#include "ConfigROMStager.hpp"
#include "ControllerStateMachine.hpp"
#include "DiagnosticLogger.hpp"
#include "HardwareInterface.hpp"
#include "InterruptManager.hpp"
#include "OHCIConstants.hpp"
#include "Logging.hpp"
#include "MetricsSink.hpp"
#include "RegisterMap.hpp"
#include "Scheduler.hpp"
#include "SelfIDCapture.hpp"
#include "TopologyManager.hpp"
#include "../Discovery/SpeedPolicy.hpp"
#include "../Discovery/ConfigROMStore.hpp"
#include "../Discovery/DeviceRegistry.hpp"
#include "../Discovery/ROMScanner.hpp"

namespace {

constexpr uint32_t kDefaultATRetries = (3u << 0) | (3u << 4) | (3u << 8) | (200u << 16);
constexpr uint32_t kDefaultNodeCapabilities = 0x00000001u;
// Per Linux ohci_enable(): defer cycleMaster until after first Self-ID
// Enabling cycleMaster here means immediate bus master election, which may not be desired
constexpr uint32_t kDefaultLinkControl = ASFW::Driver::LinkControlBits::kRcvSelfID |
                                          ASFW::Driver::LinkControlBits::kRcvPhyPkt |
                                          ASFW::Driver::LinkControlBits::kCycleTimerEnable;
                                          // Note: kCycleMaster deferred per Linux discipline
constexpr uint32_t kPostedWritePrimingBits = ASFW::Driver::HCControlBits::kPostedWriteEnable |
                                             ASFW::Driver::HCControlBits::kLPS;
[[maybe_unused]] constexpr uint32_t kAsReqAcceptAllMask = 0x80000000u;

} // namespace

namespace ASFW::Driver {

ControllerCore::ControllerCore(const ControllerConfig& config, Dependencies deps)
    : config_(config), deps_(std::move(deps)) {}

ControllerCore::~ControllerCore() {
    Stop();
}

kern_return_t ControllerCore::Start(IOService* provider) {
    if (running_) {
        return kIOReturnSuccess;
    }

    if (deps_.stateMachine) {
        deps_.stateMachine->TransitionTo(ControllerState::kStarting, "ControllerCore::Start", mach_absolute_time());
    }

    ASFW_LOG(Controller, "Sleeping for 5 seconds - Attach debugger NOW");
    IOSleep(5000);

    // FSM requires asyncSubsystem, selfIdCapture, configRomStager for actions to function
    // NEW: Add TopologyManager for building topology snapshot after Self-ID decode
    if (deps_.busReset && deps_.hardware && deps_.scheduler &&
        deps_.asyncSubsystem && deps_.selfId && deps_.configRomStager && deps_.interrupts && deps_.topology) {
        
        auto workQueue = deps_.scheduler->Queue();
        ASFW_LOG(Controller, "Initializing BusResetCoordinator: workQueue=%p (from scheduler=%p)",
                 workQueue.get(), deps_.scheduler.get());
        
        deps_.busReset->Initialize(deps_.hardware.get(),
                                  workQueue,
                                  deps_.asyncSubsystem.get(),
                                  deps_.selfId.get(),
                                  deps_.configRomStager.get(),
                                  deps_.interrupts.get(),
                                  deps_.topology.get(),
                                  deps_.romScanner.get());
        
        // Bind topology callback to trigger Discovery when topology is ready
        ASFW_LOG(Controller, "Binding topology callback for Discovery integration");
        deps_.busReset->BindCallbacks([this](const TopologySnapshot& snap) {
            this->OnTopologyReady(snap);
        });

        // Bind ROMScanner completion callback (Apple-style immediate completion)
        if (deps_.romScanner) {
            ASFW_LOG(Controller, "Binding ROMScanner completion callback (Apple pattern)");
            deps_.romScanner->SetCompletionCallback([this](Discovery::Generation gen) {
                this->OnDiscoveryScanComplete(gen);
            });
        }
    } else {
        ASFW_LOG(Controller, "❌ CRITICAL: Missing dependencies for BusResetCoordinator initialization");
        ASFW_LOG(Controller, "  busReset=%p hardware=%p scheduler=%p async=%p selfId=%p configRom=%p interrupts=%p topology=%p",
                 deps_.busReset.get(), deps_.hardware.get(), deps_.scheduler.get(),
                 deps_.asyncSubsystem.get(), deps_.selfId.get(), deps_.configRomStager.get(),
                 deps_.interrupts.get(), deps_.topology.get());
        return kIOReturnNoResources;
    }

    hardwareAttached_ = (provider != nullptr);

    // Stage hardware while interrupts remain masked. This mirrors linux firewire/ohci.c
    // ohci_enable(): the PCI IRQ is registered up front, but the controller stays
    // quiet until after configuration and Config ROM staging complete.
    // Keeping DriverKit's dispatch source disabled here prevents the soft-reset
    // induced bus reset from racing ahead of Self-ID buffer programming.
    kern_return_t kr = InitialiseHardware(provider);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Controller, "❌ Hardware initialization failed: 0x%08x", kr);
        hardwareAttached_ = false;
        if (deps_.stateMachine) {
            deps_.stateMachine->TransitionTo(ControllerState::kFailed, "ControllerCore::Start hardware init failed", mach_absolute_time());
        }
        return kr;
    }

    if (!deps_.interrupts) {
        ASFW_LOG(Controller, "❌ CRITICAL: No InterruptManager - cannot enable interrupts!");
        if (deps_.stateMachine) {
            deps_.stateMachine->TransitionTo(ControllerState::kFailed, "ControllerCore::Start missing InterruptManager", mach_absolute_time());
        }
        return kIOReturnNoResources;
    }

    // Arm the controller to receive interrupts only after the Self-ID buffer, Config ROM,
    // and link control bits are staged. This mirrors linux firewire/ohci.c:2470-2586,
    // where IntMaskSet is written immediately before linkEnable.
    running_ = true;
    ASFW_LOG(Controller, "Enabling IOInterruptDispatchSource AFTER hardware staging (Linux ordering)...");
    deps_.interrupts->Enable();
    ASFW_LOG(Controller, "✓ IOInterruptDispatchSource enabled");

    kr = EnableInterruptsAndStartBus();
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Controller, "❌ Final enable sequence failed: 0x%08x", kr);
        deps_.interrupts->Disable();
        running_ = false;
        hardwareAttached_ = false;
        if (deps_.stateMachine) {
            deps_.stateMachine->TransitionTo(ControllerState::kFailed, "ControllerCore::Start enable failed", mach_absolute_time());
        }
        return kr;
    }

    ASFW_LOG(Controller, "✓ Hardware initialization complete - interrupt delivery active");

    if (deps_.stateMachine) {
        deps_.stateMachine->TransitionTo(ControllerState::kRunning, "ControllerCore::Start complete", mach_absolute_time());
    }
    return kIOReturnSuccess;
}

void ControllerCore::Stop() {
    if (!running_) {
        return;
    }

    ASFW_LOG(Controller, "ControllerCore::Stop - beginning shutdown sequence");

    if (deps_.stateMachine) {
        deps_.stateMachine->TransitionTo(ControllerState::kQuiescing, "ControllerCore::Stop", mach_absolute_time());
    }

    // Disable interrupts FIRST to prevent new events during shutdown
    if (deps_.interrupts) {
        ASFW_LOG(Controller, "Disabling IOInterruptDispatchSource...");
        deps_.interrupts->Disable();
        ASFW_LOG(Controller, "✓ Interrupts disabled");
    }

    // Mark as not running to prevent HandleInterrupt from processing events
    running_ = false;

    if (hardwareAttached_ && deps_.hardware) {
        if (deps_.configRomStager) {
            deps_.configRomStager->Teardown(*deps_.hardware);
        }
        deps_.hardware->Detach();
        hardwareAttached_ = false;
    }

    hardwareInitialised_ = false;
    phyProgramSupported_ = false;
    phyConfigOk_ = false;

    if (deps_.stateMachine) {
        deps_.stateMachine->TransitionTo(ControllerState::kStopped, "ControllerCore::Stop complete", mach_absolute_time());
    }
    
    ASFW_LOG(Controller, "✓ ControllerCore::Stop complete");
}

void ControllerCore::HandleInterrupt(const InterruptSnapshot& snapshot) {
    if (!running_ || !deps_.hardware) {
        ASFW_LOG(Controller, "HandleInterrupt early return (running=%d hw=%p)", running_, deps_.hardware.get());
        return;
    }

    auto& hw = *deps_.hardware;
    const uint32_t rawEvents = snapshot.intEvent;

    // OHCI §5.7: IntMaskSet/IntMaskClear are write-only strobes - reading returns undefined value
    const uint32_t currentMask = deps_.interrupts ? deps_.interrupts->EnabledMask() : 0xFFFFFFFF;
    const uint32_t events = rawEvents & currentMask;

    if (rawEvents != events) {
        ASFW_LOG(Controller, "Filtered masked interrupts: raw=0x%08x enabled=0x%08x mask=0x%08x",
               rawEvents, events, currentMask);
    }

    // RAW INTERRUPT LOGGING: Log every interrupt during bus reset for diagnostics
    // This helps diagnose timing issues, missing interrupts, and hardware quirks
    if (deps_.busReset && deps_.busReset->GetState() != BusResetCoordinator::State::Idle) {
        ASFW_LOG(Controller, "🔍 BUS RESET ACTIVE - Raw interrupt: 0x%08x @ %llu ns (mask=0x%08x filtered=0x%08x)",
                 rawEvents, snapshot.timestamp, currentMask, events);
    }
    
    ASFW_LOG(Controller, "HandleInterrupt: events=0x%08x AsyncSubsystem=%p", events, deps_.asyncSubsystem.get());

    // Detailed interrupt decode (adapted from Linux log_irqs)
    const std::string eventDecode = DiagnosticLogger::DecodeInterruptEvents(events);
    ASFW_LOG(Controller, "%{public}s", eventDecode.c_str());

    // Check for critical hardware errors first
    if (events & IntEventBits::kUnrecoverableError) {
        ASFW_LOG(Controller, "❌ CRITICAL: UnrecoverableError interrupt - hardware fault detected!");
        DiagnoseUnrecoverableError();
        // TODO(ASFW-Error): Implement error recovery or halt driver
    }
    
    // Check for CSR register access failures (often occurs with UnrecoverableError)
    if (events & IntEventBits::kRegAccessFail) {
        ASFW_LOG(Controller, "❌ CRITICAL: regAccessFail - CSR register access failed!");
        ASFW_LOG(Controller, "This indicates hardware could not complete a register read/write operation");
        ASFW_LOG(Controller, "Common causes: Self-ID buffer access, Config ROM mapping, or context register access");
    }

    // Check for cycle timing errors (adapted from Linux irq handler)
    if (events & IntEventBits::kCycleTooLong) {
        ASFW_LOG(Controller, "⚠️ WARNING: Cycle too long - isochronous cycle overran 125μs budget");
        ASFW_LOG(Controller, "This indicates DMA descriptors or system latency causing timing violation");
        // Per OHCI §6.2.1: cycleTooLong fires when cycle exceeds 125μs nominal
    }
    
    // Per Linux irq_handler: postedWriteErr very often pairs with unrecoverableError
    // Most common cause: Self-ID buffer or Config ROM DMA address invalid/unmapped
    // OHCI §13.2.4: Hardware detected error during posted write DMA cycle to host memory
    if (events & IntEventBits::kPostedWriteErr) {
        ASFW_LOG(Controller, "❌ CRITICAL: Posted write error - DMA posted write to host memory failed!");
        ASFW_LOG(Controller, "This indicates IOMMU mapping error or invalid DMA target address");
        ASFW_LOG(Controller, "Common causes: Self-ID buffer DMA, Config ROM shadow update");
        // Per OHCI §13.2.4: Hardware detected error during posted write DMA cycle
    }
    
    if (events & IntEventBits::kCycle64Seconds) {
        ASFW_LOG(Controller, "Cycle64Seconds - 64-second cycle counter rollover");
    }
    
    // Feed relevant events to BusResetCoordinator FSM (it filters what it needs)
    const uint32_t busResetRelevantBits = IntEventBits::kBusReset |
                                          IntEventBits::kSelfIDComplete |
                                          IntEventBits::kSelfIDComplete2 |
                                          IntEventBits::kUnrecoverableError |
                                          IntEventBits::kRegAccessFail;
    if ((events & busResetRelevantBits) && deps_.busReset) {
        deps_.busReset->OnIrq(events & busResetRelevantBits, snapshot.timestamp);
    }

    // Dispatch AT Request completions
    if ((events & IntEventBits::kReqTxComplete) && deps_.asyncSubsystem) {
        ASFW_LOG(Controller, "AT Request complete interrupt (transmit done)");
        deps_.asyncSubsystem->OnTxInterrupt();
        // TODO(ASFW-Logging): Add DiagnosticLogger::DecodeAsyncPacket() call once we extract packet headers
    }

    // Dispatch AT Response completions
    if ((events & IntEventBits::kRespTxComplete) && deps_.asyncSubsystem) {
        ASFW_LOG(Controller, "AT Response complete interrupt (transmit done)");
        deps_.asyncSubsystem->OnTxInterrupt();
        // TODO(ASFW-Logging): Add DiagnosticLogger::DecodeAsyncPacket() call once we extract packet headers
    }

    // Dispatch AR Request interrupts (OHCI §6.1.2: RQPkt indicates packet available)
    // Use kRQPkt (bit 4), NOT kARRQ (bit 2)
    // kRQPkt = "request packet received into AR Request context"
    // kARRQ = "AR Request context active" (different semantics)
    if ((events & IntEventBits::kRQPkt) && deps_.asyncSubsystem) {
        ASFW_LOG(Controller, "AR Request interrupt (RQPkt: async receive packet available)");
        deps_.asyncSubsystem->OnRxInterrupt(ASFW::Async::AsyncSubsystem::ARContextType::Request);
        // TODO(ASFW-Logging): Add DiagnosticLogger::DecodeAsyncPacket() call once we extract packet headers
    }

    // Dispatch AR Response interrupts (OHCI §6.1.2: RSPkt indicates packet available)
    if ((events & IntEventBits::kRSPkt) && deps_.asyncSubsystem) {
        ASFW_LOG(Controller, "AR Response interrupt (RSPkt: async receive packet available)");
        deps_.asyncSubsystem->OnRxInterrupt(ASFW::Async::AsyncSubsystem::ARContextType::Response);
        // TODO(ASFW-Logging): Add DiagnosticLogger::DecodeAsyncPacket() call once we extract packet headers
    }

    if (events & IntEventBits::kBusReset) {
        ASFW_LOG(Controller, "Bus reset detected @ %llu ns", snapshot.timestamp);
        
        // Narrow the masked window: disable busReset source in top-half,
        // re-enable in FSM after event is cleared. Mirrors Linux pattern.
        if (deps_.interrupts) {
            deps_.interrupts->MaskInterrupts(&hw, IntEventBits::kBusReset);
        }
        
        // NOTE: All bus reset handling delegated to BusResetCoordinator FSM via OnIrq()
        // FSM owns: AsyncSubsystem flush/rearm, selfIDComplete2 clearing, Self-ID decode,
        // Config ROM restoration, topology updates, and metrics tracking
    }

    if (events & IntEventBits::kSelfIDComplete) {       // 0x0001_0000 bit 16
        ASFW_LOG(Hardware, "Self-ID Complete (bit16)");
        // NOTE: All Self-ID processing delegated to BusResetCoordinator FSM
    }

    if (events & IntEventBits::kSelfIDComplete2) {      // 0x0000_8000 bit 15
        ASFW_LOG(Hardware, "Self-ID Complete2 (bit15, sticky)");
        // NOTE: FSM tracks both completion phases via OnIrq() and processes accordingly
    }

    // FSM handles all of the above through proper state machine transitions
    
    // FSM handles all of the above through proper state machine transitions
    
    // Only clear non-reset events here (AR/AT completions, errors, etc.)
    uint32_t toAck = events & ~(IntEventBits::kBusReset | 
                                IntEventBits::kSelfIDComplete | 
                                IntEventBits::kSelfIDComplete2);
    if (toAck) {
        hw.ClearIntEvents(toAck);
    }
    hw.ClearIsoXmitEvents(snapshot.isoXmitEvent);
    hw.ClearIsoRecvEvents(snapshot.isoRecvEvent);
}

const ControllerStateMachine& ControllerCore::StateMachine() const {
    static ControllerStateMachine placeholder;
    return deps_.stateMachine ? *deps_.stateMachine : placeholder;
}

MetricsSink& ControllerCore::Metrics() {
    static MetricsSink placeholder{};
    return deps_.metrics ? *deps_.metrics : placeholder;
}

std::optional<TopologySnapshot> ControllerCore::LatestTopology() const {
    if (deps_.topology) {
        auto snapshot = deps_.topology->LatestSnapshot();
        if (snapshot.has_value()) {
            // mute log spamming
            // ASFW_LOG(Controller, "LatestTopology() returning snapshot: gen=%u nodes=%u root=%{public}s IRM=%{public}s",
            //          snapshot->generation,
            //          snapshot->nodeCount,
            //          snapshot->rootNodeId.has_value() ? std::to_string(*snapshot->rootNodeId).c_str() : "none",
            //          snapshot->irmNodeId.has_value() ? std::to_string(*snapshot->irmNodeId).c_str() : "none");
        } else {
            ASFW_LOG(Controller, "LatestTopology() returning nullopt (no topology built yet)");
        }
        return snapshot;
    }
    ASFW_LOG(Controller, "LatestTopology() returning nullopt (no TopologyManager)");
    return std::nullopt;
}

Discovery::ConfigROMStore* ControllerCore::GetConfigROMStore() const {
    return deps_.romStore.get();
}

Discovery::ROMScanner* ControllerCore::GetROMScanner() const {
    return deps_.romScanner.get();
}

kern_return_t ControllerCore::PerformSoftReset() {
    if (!deps_.hardware) {
        ASFW_LOG(Hardware, "No hardware interface for software reset");
        return kIOReturnNoDevice;
    }

    auto& hw = *deps_.hardware;
    ASFW_LOG(Hardware, "Performing software reset...");
    hw.SetHCControlBits(HCControlBits::kSoftReset);

    constexpr uint32_t kResetTimeoutUsec = 500'000;  // 500ms (matches Linux)
    constexpr uint32_t kResetPollUsec = 1'000;       // 1ms poll interval

    // Wait for softReset bit to CLEAR (hardware clears it when reset complete)
    const bool cleared = hw.WaitHC(HCControlBits::kSoftReset, false,
                                    kResetTimeoutUsec, kResetPollUsec);
    if (!cleared) {
        ASFW_LOG(Hardware, "Software reset timeout after 500ms");
        return kIOReturnTimeout;
    }

    ASFW_LOG(Hardware, "Software reset complete");
    return kIOReturnSuccess;
}

kern_return_t ControllerCore::InitialiseHardware(IOService* provider) {
    (void)provider;
    if (hardwareInitialised_) {
        return kIOReturnSuccess;
    }

    if (!deps_.hardware) {
        ASFW_LOG(Hardware, "No hardware interface provided");
        return kIOReturnNoDevice;
    }

    auto& hw = *deps_.hardware;
    if (!hw.Attached()) {
        ASFW_LOG(Hardware, "HardwareInterface not attached; aborting init");
        return kIOReturnNotReady;
    }

    // Reset PHY derived state each time we attempt bring-up so the final enable
    // phase can decide whether an explicit PHY initiated bus reset is required.
    phyProgramSupported_ = false;
    phyConfigOk_ = false;

    ASFW_LOG(Hardware, "═══════════════════════════════════════════════════════════");
    ASFW_LOG(Hardware, "Starting OHCI controller initialization sequence");
    ASFW_LOG(Hardware, "═══════════════════════════════════════════════════════════");

    // Step 1: Software reset - clear all controller state
    const kern_return_t resetStatus = PerformSoftReset();
    if (resetStatus != kIOReturnSuccess) {
        ASFW_LOG(Hardware, "✗ Software reset FAILED: 0x%08x", resetStatus);
        return resetStatus;
    }

    // Step 2: Clear all interrupt events and masks before initialization
    hw.ClearIntEvents(0xFFFFFFFF);
    // Keep software shadow in sync (OHCI §6.2: Set/Clear are write-only)
    if (deps_.interrupts) {
        deps_.interrupts->MaskInterrupts(&hw, 0xFFFFFFFF);
    } else {
        hw.SetInterruptMask(0xFFFFFFFF, false);
    }

    ASFW_LOG(Hardware, "Initialising OHCI core (LPS bring-up ➜ config ROM staging)");

    // Per Linux ohci_enable() lines 2428-2441: Enable LPS and poll with retry
    // Some controllers (TI TSB82AA2, ALI M5251) need multiple attempts
    hw.SetHCControlBits(kPostedWritePrimingBits);
    
    // Retry loop: 50ms × 3 attempts (matches Linux lps polling)
    bool lpsAchieved = false;
    for (int lpsRetry = 0; lpsRetry < 3; lpsRetry++) {
        IOSleep(50);  // 50ms per attempt (Linux uses msleep(50))
        const uint32_t hcControl = hw.ReadHCControl();
        if (hcControl & HCControlBits::kLPS) {
            lpsAchieved = true;
            break;
        }
    }
    
    if (!lpsAchieved) {
        const uint32_t finalHC = hw.ReadHCControl();
        ASFW_LOG(Hardware, "✗ Failed to set Link Power Status after 3 × 50ms attempts (HCControl=0x%08x)", 
                 finalHC);
        return kIOReturnTimeout;
    }

    // Additional settling time after LPS before PHY access
    // Per Linux ohci_enable(): some cards signal LPS early but cannot use
    // the PHY immediately; add a small pause before accessing PHY.
    IOSleep(50);  // Additional 50ms settling (total 100-200ms from LPS enable)

    // Step 3: Detect OHCI version
    const uint32_t version = hw.Read(Register32::kVersion);
    ohciVersion_ = version & 0x00FF00FF;  // Store for feature detection
    constexpr uint32_t kOHCI_1_1 = 0x010010;
    const bool isOHCI_1_1_OrLater = (ohciVersion_ >= kOHCI_1_1);

    // Step 3a: Enable OHCI 1.1+ features if supported
    // Linux: if (version >= OHCI_VERSION_1_1) { reg_write(ohci, OHCI1394_InitialChannelsAvailableHi, 0xfffffffe); }
    // OHCI 1.1 spec §5.5: InitialChannelsAvailableHi enables channels 32-62 for isochronous
    // Bit pattern 0xfffffffe = channels 33-63 available (bit 0 = channel 32, reserved)
    // This enables broadcast channel (63) auto-allocation behavior
    if (isOHCI_1_1_OrLater) {
        hw.WriteAndFlush(Register32::kInitialChannelsAvailableHi, 0xFFFFFFFE);
    }

    // Step 4: Clear noByteSwapData - enable byte-swapping for data phases per OHCI spec
    // Per OHCI §5.7: noByteSwapData=0 enables endianness conversion for packet data
    // macOS is little-endian, most FireWire devices expect big-endian wire format
    hw.ClearHCControlBits(HCControlBits::kNoByteSwap);
    
    // Step 5: Check if PHY register programming is allowed
    // Per OHCI §5.7.2: programPhyEnable bit indicates if generic software can configure PHY
    const uint32_t hcControlBefore = hw.ReadHCControl();
    const bool programPhyEnableSupported = (hcControlBefore & HCControlBits::kProgramPhyEnable) != 0;
    phyProgramSupported_ = programPhyEnableSupported;

    ASFW_LOG(Hardware, "HCControl=0x%08x (programPhyEnable=%{public}s)",
             hcControlBefore, programPhyEnableSupported ? "YES" : "NO");

    if (!programPhyEnableSupported) {
        ASFW_LOG(Hardware, "WARNING: programPhyEnable=0 - PHY may be pre-configured by firmware/BIOS");
        ASFW_LOG(Hardware, "Per OHCI §5.7.2: Generic software may not modify PHY configuration");
        ASFW_LOG(Hardware, "Skipping PHY register 4 configuration (PHY should already be configured)");
        // Don't fail - firmware may have already configured PHY correctly
    }

    // Step 5a: Configure PHY register 4 (enables PHY link layer)
    // Only attempt if programPhyEnable is set
    bool phyConfigOk = false;  // Track PHY configuration success for later aPhyEnhanceEnable decision
    if (programPhyEnableSupported) {
        // Per Linux configure_1394a_enhancements() (ohci.c line 2372-2389):
        // Gate+settle+probe sequence BEFORE any PHY register writes
        // PHY may not respond immediately after LPS; open gate and wait to settle
        
        // Step 1: Open PHY programming gate (per OHCI §5.7.2)
        hw.SetHCControlBits(HCControlBits::kProgramPhyEnable);
        ASFW_LOG_PHY("Opened PHY programming gate (programPhyEnable=1)");
        
        // Step 2: Settle delay for PHY sideband interface
        // Per Linux ohci_enable() TI TSB82AA2 quirk (line 2437-2440):
        // "TI TSB82AA2 + TSB81BA3(A) cards signal LPS enabled early but
        //  cannot actually use the phy at that time. These need tens of
        //  milliseconds pause between LPS write and first phy access too."
        IODelay(1000);  // ~1ms settle (matches Linux polling loop delays)
        ASFW_LOG_PHY("PHY sideband settle delay complete");
        
        // Step 3: Probe PHY (read reg1) to verify it's responding
        // If probe fails, retry once by toggling LPS (per Linux quirk handling)
        auto phyId = hw.ReadPhyRegister(1);
        if (!phyId) {
            ASFW_LOG(Hardware, "PHY probe failed on first attempt; retrying with LPS toggle");
            hw.ClearHCControlBits(HCControlBits::kLPS);
            IODelay(5000);  // 5ms LPS settle
            hw.SetHCControlBits(HCControlBits::kLPS);
            IOSleep(50);    // 50ms stabilization (per Linux TI quirk)
            phyId = hw.ReadPhyRegister(1);
        }
        
        if (!phyId) {
            ASFW_LOG(Hardware, "PHY probe failed after retry; skipping reg4 config");
            ASFW_LOG(Hardware, "Will rely on firmware/BIOS PHY configuration");
        } else {
            ASFW_LOG_PHY("PHY probe OK (reg1=0x%02x); proceeding with configuration", phyId.value());
            
            // Step 4: Configure PHY register 4
            // Per Linux ohci_enable() line 2511: "Activate link_on bit and contender bit in our self ID packets"
            // PHY_LINK_ACTIVE (0x80) - enables PHY link layer - REQUIRED for PHY to respond to operations!
            // PHY_CONTENDER (0x40) - marks this node as a bus master contender
            // IEEE 1394a-2000 §4.3.4.1: PHY register 4 controls link layer and contender status
            constexpr uint8_t kPhyLinkActive = 0x80;  // Bit 7: link_on
            constexpr uint8_t kPhyContender = 0x40;   // Bit 6: contender
            constexpr uint8_t kPhyReg4Address = 4;

            ASFW_LOG_PHY("Configuring PHY register 4 (link_on + contender)");
            phyConfigOk = hw.UpdatePhyRegister(kPhyReg4Address,
                                                0,  // clearBits
                                                kPhyLinkActive | kPhyContender);  // setBits
            if (!phyConfigOk) {
                ASFW_LOG(Hardware, "PHY reg4 write failed (unexpected after successful probe)");
            } else {
                ASFW_LOG_PHY("PHY reg4 configured: link_on=1 contender=1");
            }
        }
    }

    phyConfigOk_ = phyConfigOk;

    // Step 5b: Finalize PHY-Link enhancement configuration (OHCI §5.7.2 + §5.7.3)
    // Per OHCI §5.7.2: "Software should clear programPhyEnable once the PHY and Link
    // have been programmed consistently." and §5.7.3: "PHY-Link enhancements shall be programmed
    // only when HCControl.linkEnable is 0."
    //
    // Per Linux configure_1394a_enhancements() (ohci.c lines 2372-2389):
    //   1. If programPhyEnable=1 → we MUST configure PHY+Link consistently
    //   2. Set/clear aPhyEnhanceEnable to match PHY IEEE1394a capability
    //   3. Clear programPhyEnable to signal configuration complete
    //
    // Note: Previously programPhyEnable was not cleared, leaving hardware in configuration
    // mode which could cause undefined behavior per OHCI §5.7.2 and trigger faults.
    if (programPhyEnableSupported) {
        // Decide aPhyEnhanceEnable state based on PHY configuration success
        // If PHY config succeeded → assume IEEE1394a PHY → enable Link enhancements
        // If PHY config failed → assume legacy PHY or firmware-configured → disable Link enhancements for safety
        if (phyConfigOk) {
            hw.SetHCControlBits(HCControlBits::kAPhyEnhanceEnable);
        } else {
            hw.ClearHCControlBits(HCControlBits::kAPhyEnhanceEnable);
            ASFW_LOG(Hardware, "aPhyEnhanceEnable CLEARED - IEEE1394a enhancements disabled in Link (PHY config failed/skipped)");
        }

        // Clear programPhyEnable to signal configuration complete
        // Per OHCI §5.7.2: "Software should clear programPhyEnable once the PHY and Link
        // have been programmed consistently."
        // Per Linux ohci.c line 2387: "Clean up: configuration has been taken care of."
        hw.ClearHCControlBits(HCControlBits::kProgramPhyEnable);

        const uint32_t hcControlAfter = hw.ReadHCControl();
        ASFW_LOG(Hardware, "HCControl after PHY/Link config: 0x%08x (programPhyEnable=%d aPhyEnhanceEnable=%d)",
                 hcControlAfter,
                 (hcControlAfter & HCControlBits::kProgramPhyEnable) ? 1 : 0,
                 (hcControlAfter & HCControlBits::kAPhyEnhanceEnable) ? 1 : 0);
    }

    // Step 6: Stage Config ROM BEFORE enabling link (OHCI §5.5.6 compliance)
    // This ensures the shadow register (ConfigROMmapNext) is loaded before
    // the auto bus reset from linkEnable activation occurs.
    const uint32_t busOptions = hw.Read(Register32::kBusOptions);
    const uint32_t guidHi = hw.Read(Register32::kGUIDHi);
    const uint32_t guidLo = hw.Read(Register32::kGUIDLo);

    const kern_return_t configRomStatus = StageConfigROM(busOptions, guidHi, guidLo);
    if (configRomStatus != kIOReturnSuccess) {
        ASFW_LOG(Hardware, "Config ROM staging failed: 0x%08x", configRomStatus);
        return configRomStatus;
    }

    // Step 7: Set Physical Upper Bound (256MB CSR address range)
    // TODO: investigate if this is required or only needed for remote DMA
# if 0
    hw.WriteAndFlush(Register32::kPhyUpperBound, 0xFFFF);
    ASFW_LOG(Hardware, "PhyUpperBound set to 0xFFFF (256MB)");
# endif

    // Per Linux ohci_enable(): Don't pre-write NodeID; bus reset will assign it from Self-ID
    // The kProvisionalNodeId value would be immediately overwritten anyway
    hw.SetLinkControlBits(kDefaultLinkControl);
    ASFW_LOG(Hardware, "LinkControl: rcvSelfID | rcvPhyPkt | cycleTimerEnable (cycleMaster deferred)");
    hw.WriteAndFlush(Register32::kAsReqFilterHiSet, kAsReqAcceptAllMask);
    
    // Build full 32-bit value explicitly per OHCI spec:
    // [31:24]=reserved(0), [23:16]=cycleLimit, [15:8]=maxPhys, [7:4]=maxResp, [3:0]=maxReq
    const uint32_t atRetriesVal = (static_cast<uint32_t>(200) << 16) |  // cycleLimit [23:16]
                                   (static_cast<uint32_t>(3) << 8) |       // maxPhys [15:8]
                                   (static_cast<uint32_t>(3) << 4) |      // maxResp [7:4]
                                   (static_cast<uint32_t>(3) << 0);       // maxReq [3:0]
    
    // Write ATRetries AFTER cycle timer enable (ensures top byte sticks)
    hw.WriteAndFlush(Register32::kATRetries, atRetriesVal);
    // Force readback to flush write pipeline
    const uint32_t atRetriesReadback = hw.Read(Register32::kATRetries);
    ASFW_LOG(Hardware, "ATRetries configured: maxReq=3 maxResp=3 maxPhys=3 cycleLimit=200");
    ASFW_LOG(Hardware, "ATRetries write/readback: 0x%08x / 0x%08x", atRetriesVal, atRetriesReadback);

    // Bus timing state: mark cycle timer as inactive during init
    // Linux: ohci->bus_time_running = false;
    // Ensures init path doesn't assume active isochronous timing
    busTimeRunning_ = false;
    ASFW_LOG(Hardware, "Bus time marked inactive - isochronous cycle timer not yet running");

    // Clear multi-channel mode on all IR contexts for clean initialization
    // Detect how many IR contexts hardware supports (read IsoRecvIntMaskSet)
    const uint32_t irContextSupport = hw.Read(Register32::kIsoRecvIntMaskSet);
    uint32_t irContextsCleared = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        if (irContextSupport & (1u << i)) {
            const uint32_t ctrlClearReg = DMAContextHelpers::IsoRcvContextControlClear(i);
            hw.WriteAndFlush(static_cast<Register32>(ctrlClearReg),
                           DMAContextHelpers::kIRContextMultiChannelMode);
            ++irContextsCleared;
        }
    }
    ASFW_LOG(Hardware, "⚠️  TODO: ISOCHRONOUS DMA STACK REQUIRED ⚠️");
    ASFW_LOG(Hardware, "Cleared multi-channel mode on %u IR contexts (support=0x%08x)",
             irContextsCleared, irContextSupport);
    ASFW_LOG(Hardware, "IR contexts ready for isochronous receive allocation (stack not yet implemented)");

    // Allocate and map Self-ID DMA buffer before arming
    // Per OHCI §11: hardware DMAs Self-ID packets to the buffer pointed to by SelfIDBuffer.
    // Per OHCI §13.2.5: an invalid/unmapped buffer address causes UnrecoverableError.
    // Sequence:
    //   1. PrepareBuffers() - allocate IOBufferMemoryDescriptor, map DMA, set segment valid
    //   2. Arm() - write valid DMA address to kSelfIDBuffer register
    // The buffer must be prepared before Arm() to avoid DMA errors during Self-ID completion.
    if (deps_.selfId) {
        // Allocate DMA buffer for Self-ID packets (512 quadlets = 2048 bytes, enough for 64 nodes)
        const kern_return_t prepStatus = deps_.selfId->PrepareBuffers(512, hw);
        if (prepStatus != kIOReturnSuccess) {
            ASFW_LOG(Hardware, "Self-ID PrepareBuffers failed: 0x%08x (DMA allocation failed)", prepStatus);
            return prepStatus;
        }
        // OHCI §11.2 requires SelfIDBuffer to contain a valid DMA address before linkEnable
        // triggers the first bus reset. Linux ohci_enable() (firewire/ohci.c:2471) programs the
        // register immediately after allocation; we mirror that here so the soft-reset induced
        // bus reset cannot DMA into address 0 and leave stale generation metadata behind.
        const kern_return_t armStatus = deps_.selfId->Arm(hw);
        if (armStatus != kIOReturnSuccess) {
            ASFW_LOG(Hardware, "Self-ID Arm failed: 0x%08x", armStatus);
            return armStatus;
        }
        ASFW_LOG(Hardware, "Self-ID buffer armed prior to first bus reset (per OHCI §11.2 / linux ohci_enable)");
    }
    return kIOReturnSuccess;
}

kern_return_t ControllerCore::EnableInterruptsAndStartBus() {
    // log entery
    ASFW_LOG(Hardware, "Entering ControllerCore::EnableInterruptsAndStartBus() ");
    if (hardwareInitialised_) {
        return kIOReturnSuccess;
    }
    if (!deps_.hardware) {
        ASFW_LOG(Hardware, "EnableInterruptsAndStartBus: no hardware interface");
        return kIOReturnNoDevice;
    }

    auto& hw = *deps_.hardware;

    // Seed IntMask with baseline policy + masterIntEnable
    // Per OHCI §5.7: After reset, IntMask is undefined and masterIntEnable=0.
    // Clear any stale state, then establish deterministic baseline.
    hw.Write(Register32::kIntMaskClear, 0xFFFFFFFFu);   // Clear all mask bits
    hw.Write(Register32::kIntEventClear, 0xFFFFFFFFu);  // Clear all pending events
    
    const uint32_t initialMask = kBaseIntMask | IntMaskBits::kMasterIntEnable;
    hw.Write(Register32::kIntMaskSet, initialMask);
    if (deps_.interrupts) {
        deps_.interrupts->EnableInterrupts(initialMask);  // Update software shadow
    }
    ASFW_LOG(Hardware, "IntMask seeded: base|master=0x%08x (busReset=%d master=%d)",
             initialMask,
             (initialMask >> 17) & 1,
             (initialMask >> 31) & 1);

    // LinkEnable + BIBimageValid must be asserted atomically once the Config ROM
    // has been staged. OHCI §5.7.3 notes this transition triggers a bus reset, so
    // we wait until interrupts are armed to avoid missing Self-ID events.
    ASFW_LOG(Hardware, "Setting linkEnable + BIBimageValid atomically - will trigger auto bus reset");
    hw.SetHCControlBits(HCControlBits::kLinkEnable | HCControlBits::kBibImageValid);
    ASFW_LOG(Hardware, "HCControl.linkEnable + BIBimageValid set - auto bus reset should initiate (OHCI §5.7.3)");

    // Some controllers require an explicit PHY initiated reset to kick the
    // Config ROM shadow. Follow linux logic by only attempting if the PHY was
    // responsive during configuration.
    if (phyProgramSupported_ && phyConfigOk_) {
        ASFW_LOG(Hardware, "Forcing bus reset via PHY to guarantee Config ROM shadow activation");
        const bool forced = hw.InitiateBusReset(false);  // long reset per OHCI §7.2.3.1
        if (!forced) {
            ASFW_LOG(Hardware, "WARNING: Forced bus reset failed; will rely on auto reset");
        } else {
            ASFW_LOG(Hardware, "Bus reset initiated via PHY control - shadow update will occur");
        }
    } else {
        ASFW_LOG(Hardware, "Skipping forced reset (PHY not confirmed); relying on auto reset from linkEnable");
    }
    ASFW_LOG_CONFIG_ROM("Config ROM shadow update will complete during bus reset (OHCI §5.5.6)");

    // Phase 2B: arm Async receive contexts now that the link is live. Requests
    // will remain quiescent until the FSM finishes the first reset cycle.
    if (deps_.asyncSubsystem) {
        const kern_return_t armStatus = deps_.asyncSubsystem->ArmARContextsOnly();
        if (armStatus != kIOReturnSuccess) {
            ASFW_LOG(Hardware, "Failed to arm AR contexts: 0x%08x", armStatus);
            return armStatus;
        }
        ASFW_LOG(Hardware, "AR contexts armed successfully (receive enabled, transmit disabled)");
    } else {
        ASFW_LOG(Controller, "No AsyncSubsystem - DMA contexts not armed");
    }

    hardwareInitialised_ = true;

    const bool linkEnabled = (hw.ReadHCControl() & HCControlBits::kLinkEnable) != 0;
    const uint32_t configRomMap = hw.Read(Register32::kConfigROMMap);
    const char* selfIdState = deps_.selfId ? "armed" : "missing";
    const char* asyncState = deps_.asyncSubsystem ? "armed" : "missing";

    ASFW_LOG(Hardware,
             "OHCI init complete: version=0x%08x link=%{public}s configROM=0x%08x selfID=%{public}s async=%{public}s",
             ohciVersion_,
             linkEnabled ? "enabled" : "disabled",
             configRomMap,
             selfIdState,
             asyncState);

    return kIOReturnSuccess;
}

kern_return_t ControllerCore::StageConfigROM(uint32_t busOptions, uint32_t guidHi, uint32_t guidLo) {
    if (!deps_.configRom || !deps_.configRomStager || !deps_.hardware) {
        ASFW_LOG(Hardware, "Config ROM dependencies missing (builder=%p stager=%p hw=%p)",
                     deps_.configRom.get(), deps_.configRomStager.get(), deps_.hardware.get());
        return kIOReturnNotReady;
    }

    auto builder = deps_.configRom;
    const uint64_t hardwareGuid = (static_cast<uint64_t>(guidHi) << 32) | static_cast<uint64_t>(guidLo);
    const uint64_t effectiveGuid = (config_.localGuid != 0) ? config_.localGuid : hardwareGuid;

    builder->Build(busOptions, effectiveGuid, kDefaultNodeCapabilities, config_.vendor.vendorName);
    if (builder->QuadletCount() < 5) {
        ASFW_LOG(Hardware, "Config ROM builder produced insufficient quadlets (%zu)",
                     builder->QuadletCount());
        return kIOReturnInternalError;
    }

    auto& hw = *deps_.hardware;
    const kern_return_t kr = deps_.configRomStager->StageImage(*builder, hw);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Hardware, "Config ROM staging failed: 0x%08x", kr);
    }
    return kr;
}

void ControllerCore::DiagnoseUnrecoverableError() {
    if (!deps_.hardware) {
        return;
    }

    auto& hw = *deps_.hardware;
    
    struct ContextInfo {
        const char* shortName;
        uint32_t controlSetReg;
    };

    const ContextInfo contexts[] = {
        {"ATreq", DMAContextHelpers::AsReqTrContextControlSet},
        {"ATrsp", DMAContextHelpers::AsRspTrContextControlSet},
        {"ARreq", DMAContextHelpers::AsReqRcvContextControlSet},
        {"ARrsp", DMAContextHelpers::AsRspRcvContextControlSet},
    };

    std::string contextSummary;
    contextSummary.reserve(64);

    bool anyDead = false;
    for (const auto& ctx : contexts) {
        const uint32_t control = hw.Read(static_cast<Register32>(ctx.controlSetReg));
        const bool dead = (control & kContextControlDeadBit) != 0;
        const uint8_t eventCode = static_cast<uint8_t>(control & kContextControlEventMask);

        if (!contextSummary.empty()) {
            contextSummary.append(" ");
        }

        contextSummary.append(ctx.shortName);
        contextSummary.append("=");

        if (dead) {
            anyDead = true;
            const auto codeEnum = static_cast<ASFW::Async::OHCIEventCode>(eventCode);
            const char* codeName = ASFW::Async::ToString(codeEnum);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "DEAD(0x%02x:%{public}s)", eventCode, codeName);
            contextSummary.append(buf);
        } else {
            contextSummary.append("OK");
        }
    }

    if (!anyDead) {
        contextSummary.append(" all-ok");
    }

    const uint32_t hcControl = hw.Read(Register32::kHCControl);
    const bool bibValid = (hcControl & HCControlBits::kBibImageValid) != 0;
    const bool linkEnable = (hcControl & HCControlBits::kLinkEnable) != 0;
    const uint32_t selfIDBufferReg = hw.Read(Register32::kSelfIDBuffer);
    const uint32_t selfIDCountReg = hw.Read(Register32::kSelfIDCount);

    ASFW_LOG(Controller,
             "UnrecoverableError contexts: %{public}s HCControl=0x%08x(BIB=%d link=%d) SelfIDBuffer=0x%08x SelfIDCount=0x%08x",
             contextSummary.c_str(), hcControl, bibValid, linkEnable, selfIDBufferReg, selfIDCountReg);

    if (!bibValid) {
        ASFW_LOG(Controller, "  BIBimageValid cleared: Config ROM fetch failure suspected");
    }

    if (selfIDBufferReg == 0) {
        ASFW_LOG(Controller, "  Self-ID buffer register is zero (not armed)");
    }
}

// ============================================================================
// Discovery Integration
// ============================================================================

namespace {
const char* DeviceKindString(Discovery::DeviceKind kind) {
    using Discovery::DeviceKind;
    switch (kind) {
        case DeviceKind::AV_C: return "AV/C";
        case DeviceKind::TA_61883: return "TA 61883 (AMDTP)";
        case DeviceKind::VendorSpecificAudio: return "Vendor Audio";
        case DeviceKind::Storage: return "Storage";
        case DeviceKind::Camera: return "Camera";
        default: return "Unknown";
    }
}
} // anonymous namespace

void ControllerCore::OnTopologyReady(const TopologySnapshot& snap) {
    if (!deps_.romScanner) {
        ASFW_LOG(Discovery, "OnTopologyReady: no ROMScanner available");
        return;
    }
    
    const uint8_t localNodeId = snap.localNodeId.value_or(0xFF);
    if (localNodeId == 0xFF) {
        ASFW_LOG(Discovery, "OnTopologyReady: invalid local node ID");
        return;
    }
    
    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");
    ASFW_LOG(Discovery, "Topology ready gen=%u, starting ROM scan for %u nodes",
             snap.generation, snap.nodeCount);
    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");
    
    deps_.romScanner->Begin(snap.generation, snap, localNodeId);
    
    // Schedule polling to check for completion
    ScheduleDiscoveryPoll(snap.generation);
}

void ControllerCore::ScheduleDiscoveryPoll(Discovery::Generation gen) {
    if (!deps_.scheduler) {
        ASFW_LOG(Discovery, "ScheduleDiscoveryPoll: no scheduler available");
        return;
    }
    
    // Schedule poll in 100ms using DispatchAsync
    deps_.scheduler->DispatchAsync([this, gen]() {
        IOSleep(100);  // 100ms delay
        PollDiscovery(gen);
    });
}

void ControllerCore::PollDiscovery(Discovery::Generation gen) {
    if (!deps_.romScanner) {
        return;
    }
    
    if (!deps_.romScanner->IsIdleFor(gen)) {
        // Still scanning, reschedule
        ASFW_LOG(Discovery, "ROM scan still in progress for gen=%u, rescheduling...", gen);
        ScheduleDiscoveryPoll(gen);
        return;
    }
    
    // Scan complete - drain results
    ASFW_LOG(Discovery, "ROM scan complete for gen=%u, draining results", gen);
    OnDiscoveryScanComplete(gen);
}

void ControllerCore::OnDiscoveryScanComplete(Discovery::Generation gen) {
    if (!deps_.romScanner || !deps_.romStore || !deps_.deviceRegistry || !deps_.speedPolicy) {
        ASFW_LOG(Discovery, "OnDiscoveryScanComplete: missing Discovery dependencies");
        return;
    }
    
    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");
    ASFW_LOG(Discovery, "ROM scan complete for gen=%u, processing results...", gen);
    
    auto roms = deps_.romScanner->DrainReady(gen);
    ASFW_LOG(Discovery, "Discovered %zu ROMs", roms.size());
    
    for (const auto& rom : roms) {
        // Store ROM
        deps_.romStore->Insert(rom);
        
        // Get link policy
        auto policy = deps_.speedPolicy->ForNode(rom.nodeId);
        
        // Upsert device
        auto& device = deps_.deviceRegistry->UpsertFromROM(rom, policy);
        
        // Log result (detailed)
        ASFW_LOG(Discovery, "═══════════════════════════════════════");
        ASFW_LOG(Discovery, "Device Discovered:");
        ASFW_LOG(Discovery, "  GUID: 0x%016llx", device.guid);
        ASFW_LOG(Discovery, "  Vendor: 0x%06x", device.vendorId);
        ASFW_LOG(Discovery, "  Model: 0x%06x", device.modelId);
        ASFW_LOG(Discovery, "  Node: %u (gen=%u)", rom.nodeId, rom.gen);
        ASFW_LOG(Discovery, "  Kind: %{public}s", DeviceKindString(device.kind));
        ASFW_LOG(Discovery, "  Audio Candidate: %{public}s", device.isAudioCandidate ? "YES" : "NO");
    }
    
    ASFW_LOG(Discovery, "═══════════════════════════════════════");
    ASFW_LOG(Discovery, "Discovery complete: %zu devices processed in gen=%u",
             roms.size(), gen);
    ASFW_LOG(Discovery, "═══════════════════════════════════════════════════════");
}

} // namespace ASFW::Driver
