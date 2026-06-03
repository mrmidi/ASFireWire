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
#include "../Bus/CSR/BroadcastChannelCSR.hpp"
#include "../Bus/CSR/TopologyMapService.hpp"
#include "../Bus/CSR/SpeedMapService.hpp"
#include "../Bus/BusManager/BusManagerElectionDriver.hpp"
#include "../Bus/BusManager/BusManagerPolicyCoordinator.hpp"
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
#include "../Protocols/AVC/CMP/CMPClient.hpp"
#include "../Protocols/Audio/DeviceProtocolFactory.hpp"
#include "../Scheduling/Scheduler.hpp"
#include "../Version/DriverVersion.hpp"
#include "BringupOverrides.hpp"
#include "ControllerStateMachine.hpp"
#include "../Logging/Logging.hpp"

namespace {
// NOTE: OHCI hardware constants moved to OHCIConstants.hpp

kern_return_t EnableLinkPowerStatus(ASFW::Driver::HardwareInterface& hw) {
    hw.SetHCControlBits(ASFW::Driver::kPostedWritePrimingBits);

    bool lpsAchieved = false;
    for (int lpsRetry = 0; lpsRetry < 3; lpsRetry++) {
        IOSleep(50);
        const uint32_t hcControl = hw.ReadHCControl();
        if ((hcControl & ASFW::Driver::HCControlBits::kLPS) != 0U) {
            lpsAchieved = true;
            break;
        }
    }

    if (!lpsAchieved) {
        const uint32_t finalHC = hw.ReadHCControl();
        ASFW_LOG(Hardware,
                 "✗ Failed to set Link Power Status after 3 × 50ms attempts (HCControl=0x%08x)",
                 finalHC);
        return kIOReturnTimeout;
    }

    IOSleep(50);
    return kIOReturnSuccess;
}

void ConfigureGapCount(ASFW::Driver::HardwareInterface& hw, uint8_t reg1Value) {
    const uint8_t kTargetGap = ASFW::Driver::kPhyGapCountMask;
    const uint8_t clearReg1Bits = ASFW::Driver::kPhyGapCountMask |
                                  ASFW::Driver::kPhyRootHoldOff |
                                  ASFW::Driver::kPhyInitiateBusReset;
    const uint8_t newReg1 =
        static_cast<uint8_t>((reg1Value & ~clearReg1Bits) | kTargetGap);

    if (newReg1 == reg1Value) {
        ASFW_LOG_PHY("PHY Register 1 already gap=0x%02x with RHB/IBR clear; skipping cold-init write",
                     kTargetGap);
        return;
    }

    ASFW_LOG_PHY("Updating PHY Gap Count (Reg 1): 0x%02x -> 0x%02x (RHB/IBR clear)",
                 reg1Value, newReg1);

    constexpr int kMaxPhyWriteAttempts = 3;
    for (int attempt = 0; attempt < kMaxPhyWriteAttempts; ++attempt) {
        if (!hw.WritePhyRegister(1, newReg1)) {
            ASFW_LOG_PHY("PHY write attempt %d failed (writePhyRegister returned false)",
                         attempt + 1);
            IOSleep(1);
            continue;
        }

        IODelay(2000);

        const auto verify = hw.ReadPhyRegister(1);
        if (verify && ((*verify & ASFW::Driver::kPhyGapCountMask) == kTargetGap) &&
            ((*verify & ASFW::Driver::kPhyRootHoldOff) == 0)) {
            ASFW_LOG_PHY("✅ PHY Gap Count confirmed: 0x%02x -> 0x%02x (attempt %d)",
                         reg1Value, *verify, attempt + 1);
            return;
        }

        ASFW_LOG_PHY("PHY gap write verify failed on attempt %d (readback=0x%02x)",
                     attempt + 1, verify.value_or(0));
        hw.ClearHCControlBits(ASFW::Driver::HCControlBits::kLPS);
        IODelay(5);
        hw.SetHCControlBits(ASFW::Driver::HCControlBits::kLPS);
        IOSleep(5);
    }

    ASFW_LOG(Hardware,
             "Failed to reliably write PHY Register 1 (gap count) after %d attempts",
             kMaxPhyWriteAttempts);
}

bool ConfigurePhyOperationalRegisters(ASFW::Driver::HardwareInterface& hw,
                                      const ASFW::Driver::ControllerConfig& config,
                                      const ASFW::Driver::RolePolicy& policy) {
    // ClientOnly is intentionally absent here: a pure client that advertises no
    // BM/IRM capability also does NOT set the Self-ID/PHY contender bit, so it can
    // never win an IRM/BM election. ServiceContext now seeds the live driver with
    // FullBusManager/ForceRootAllowed for hardware validation, matching the
    // reference stacks' contender posture and exercising root/gap BM duties.
    // cross-validated with Linux: ohci.c:2510-2511
    const bool shouldAdvertiseContender =
        (policy.roleMode == ASFW::FW::RoleMode::FullBusManager &&
         policy.fullBMActivityLevel >= ASFW::FW::FullBMActivityLevel::ElectionOnly) ||
        policy.roleMode == ASFW::FW::RoleMode::IRMResourceHost ||
        (policy.roleMode == ASFW::FW::RoleMode::LegacyBmcCleared &&
         config.allowCycleMasterEligibility);

    const uint8_t phyReg4Bits =
        shouldAdvertiseContender
            ? (ASFW::Driver::kPhyLinkActive | ASFW::Driver::kPhyContender)
            : ASFW::Driver::kPhyLinkActive;

    ASFW_LOG_PHY("Configuring PHY register 4 (link_on=1 contender=%d)",
                 shouldAdvertiseContender ? 1 : 0);
    const uint8_t clearReg4Bits =
        shouldAdvertiseContender ? uint8_t{0} : ASFW::Driver::kPhyContender;
    bool phyConfigOk = hw.UpdatePhyRegister(ASFW::Driver::kPhyReg4Address,
                                            clearReg4Bits,
                                            phyReg4Bits);
    if (!phyConfigOk) {
        ASFW_LOG(Hardware, "Failed to configure PHY register 4");
        return false;
    }

    ASFW_LOG_PHY("PHY reg4 configured: link_on=1 contender=%d",
                 shouldAdvertiseContender ? 1 : 0);
    hw.InitializePhyReg4Cache();

    const uint8_t phyReg5EnhanceBits =
        ASFW::Driver::kPhyEnableAcceleration | ASFW::Driver::kPhyEnableMulti;
    const bool accelEnabled =
        hw.UpdatePhyRegister(ASFW::Driver::kPhyReg5Address, 0, phyReg5EnhanceBits);
    if (!accelEnabled) {
        ASFW_LOG(Hardware, "Failed to enable PHY accelerated/multi arbitration (reg5 bits 1:0)");
        return false;
    }

    ASFW_LOG_PHY("PHY reg5 configured: Enab_accel=1 Enab_multi=1");
    return true;
}

bool ConfigurePhyRegisters(ASFW::Driver::HardwareInterface& hw,
                           const ASFW::Driver::ControllerConfig& config,
                           const ASFW::Driver::RolePolicy& policy) {
    hw.SetHCControlBits(ASFW::Driver::HCControlBits::kProgramPhyEnable);
    ASFW_LOG_PHY("Opened PHY programming gate (programPhyEnable=1)");
    IODelay(1000);

    auto phyId = hw.ReadPhyRegister(1);
    if (!phyId) {
        ASFW_LOG(Hardware, "PHY probe failed on first attempt; retrying with LPS toggle");
        hw.ClearHCControlBits(ASFW::Driver::HCControlBits::kLPS);
        IODelay(5000);
        hw.SetHCControlBits(ASFW::Driver::HCControlBits::kLPS);
        IOSleep(50);
        phyId = hw.ReadPhyRegister(1);
    }

    if (!phyId) {
        ASFW_LOG(Hardware, "PHY probe failed after retry; relying on firmware defaults");
        return false;
    }

    const uint8_t reg1Value = phyId.value();
    ASFW_LOG_PHY("PHY probe OK (reg1=0x%02x)", reg1Value);
    ConfigureGapCount(hw, reg1Value);
    return ConfigurePhyOperationalRegisters(hw, config, policy);
}

void FinalizePhyLinkConfiguration(ASFW::Driver::HardwareInterface& hw,
                                  bool programPhyEnableSupported,
                                  bool phyConfigOk) {
    if (!programPhyEnableSupported) {
        return;
    }

    if (phyConfigOk) {
        hw.SetHCControlBits(ASFW::Driver::HCControlBits::kAPhyEnhanceEnable);
    } else {
        hw.ClearHCControlBits(ASFW::Driver::HCControlBits::kAPhyEnhanceEnable);
        ASFW_LOG(Hardware, "aPhyEnhanceEnable CLEARED - IEEE1394a enhancements disabled in "
                           "Link (PHY config failed/skipped)");
    }

    hw.ClearHCControlBits(ASFW::Driver::HCControlBits::kProgramPhyEnable);

    const uint32_t hcControlAfter = hw.ReadHCControl();
    ASFW_LOG(
        Hardware,
        "HCControl after PHY/Link config: 0x%08x (programPhyEnable=%d aPhyEnhanceEnable=%d)",
        hcControlAfter, (hcControlAfter & ASFW::Driver::HCControlBits::kProgramPhyEnable) ? 1 : 0,
        (hcControlAfter & ASFW::Driver::HCControlBits::kAPhyEnhanceEnable) ? 1 : 0);
}

void ConfigureAtRetries(ASFW::Driver::HardwareInterface& hw) {
    const uint32_t atRetriesVal = ASFW::Driver::kDefaultATRetries;
    hw.WriteAndFlush(ASFW::Driver::Register32::kATRetries, atRetriesVal);
    const uint32_t atRetriesReadback = hw.Read(ASFW::Driver::Register32::kATRetries);
    ASFW_LOG(Hardware, "ATRetries configured: maxReq=15 maxResp=2 maxPhys=8 cycleLimit=200");
    ASFW_LOG(Hardware, "ATRetries write/readback: 0x%08x / 0x%08x", atRetriesVal,
             atRetriesReadback);
}

void ClearIsoReceiveMultiChannelMode(ASFW::Driver::HardwareInterface& hw) {
    const uint32_t irContextSupport = hw.Read(ASFW::Driver::Register32::kIsoRecvIntMaskSet);
    uint32_t irContextsCleared = 0;
    for (uint32_t i = 0; i < 32; ++i) {
        if ((irContextSupport & (1U << i)) != 0U) {
            const uint32_t ctrlClearReg = DMAContextHelpers::IsoRcvContextControlClear(i);
            hw.WriteAndFlush(ASFW::Driver::Register32FromOffsetUnchecked(ctrlClearReg),
                             DMAContextHelpers::kIRContextMultiChannelMode);
            ++irContextsCleared;
        }
    }

    ASFW_LOG(Hardware, "Isochronous DMA stack is still required before this path is enabled");
    ASFW_LOG(Hardware, "Cleared multi-channel mode on %u IR contexts (support=0x%08x)",
             irContextsCleared, irContextSupport);
    ASFW_LOG(Hardware,
             "IR contexts ready for isochronous receive allocation (stack not yet implemented)");
}

kern_return_t PrepareSelfIdBuffer(const std::shared_ptr<ASFW::Driver::SelfIDCapture>& selfId,
                                  ASFW::Driver::HardwareInterface& hw) {
    if (!selfId) {
        return kIOReturnSuccess;
    }

    const kern_return_t prepStatus = selfId->PrepareBuffers(512, hw);
    if (prepStatus != kIOReturnSuccess) {
        ASFW_LOG(Hardware, "Self-ID PrepareBuffers failed: 0x%08x (DMA allocation failed)",
                 prepStatus);
        return prepStatus;
    }

    const kern_return_t armStatus = selfId->Arm(hw);
    if (armStatus != kIOReturnSuccess) {
        ASFW_LOG(Hardware, "Self-ID Arm failed: 0x%08x", armStatus);
        return armStatus;
    }

    ASFW_LOG(
        Hardware,
        "Self-ID buffer armed prior to first bus reset (per OHCI §11.2 / linux ohci_enable)");
    return kIOReturnSuccess;
}

void SeedInitialInterruptMask(ASFW::Driver::HardwareInterface& hw,
                              ASFW::Driver::InterruptManager* interrupts) {
    hw.Write(ASFW::Driver::Register32::kIntMaskClear, 0xFFFFFFFFU);
    hw.Write(ASFW::Driver::Register32::kIntEventClear, 0xFFFFFFFFU);

    const uint32_t initialMask =
        ASFW::Driver::kBaseIntMask | ASFW::Driver::IntMaskBits::kMasterIntEnable;
    hw.Write(ASFW::Driver::Register32::kIntMaskSet, initialMask);
    if (interrupts) {
        interrupts->EnableInterrupts(initialMask);
    }
    ASFW_LOG(Hardware, "IntMask seeded: base|master=0x%08x", initialMask);
}

void MaybeForceInitialBusReset(ASFW::Driver::HardwareInterface& hw,
                               bool phyProgramSupported,
                               bool phyConfigOk) {
    if (phyProgramSupported && phyConfigOk) {
        ASFW_LOG(Hardware, "Forcing bus reset via PHY to guarantee Config ROM shadow activation");
        const bool forced = hw.InitiateBusReset(false);
        if (!forced) {
            ASFW_LOG(Hardware, "WARNING: Forced bus reset failed; will rely on auto reset");
        }
        return;
    }

    ASFW_LOG(Hardware, "Skipping forced reset; relying on auto reset from linkEnable");
}

kern_return_t ArmAsyncReceiveContexts(ASFW::Async::IAsyncControllerPort* asyncController) {
    if (!asyncController) {
        ASFW_LOG(Controller, "No AsyncSubsystem - DMA contexts not armed");
        return kIOReturnSuccess;
    }

    const kern_return_t armStatus = asyncController->ArmARContextsOnly();
    if (armStatus != kIOReturnSuccess) {
        ASFW_LOG(Hardware, "Failed to arm AR contexts: 0x%08x", armStatus);
        return armStatus;
    }

    ASFW_LOG(Hardware, "AR contexts armed successfully");
    return kIOReturnSuccess;
}

void LogInitSummary(ASFW::Driver::HardwareInterface& hw,
                    uint32_t ohciVersion,
                    const std::shared_ptr<ASFW::Driver::SelfIDCapture>& selfId,
                    const std::shared_ptr<ASFW::Async::IAsyncControllerPort>& asyncController) {
    const bool linkEnabled = (hw.ReadHCControl() & ASFW::Driver::HCControlBits::kLinkEnable) != 0;
    const uint32_t configRomMap = hw.Read(ASFW::Driver::Register32::kConfigROMMap);
    const char* selfIdState = selfId ? "armed" : "missing";
    const char* asyncState = asyncController ? "armed" : "missing";

    ASFW_LOG(Hardware,
             "OHCI init complete: version=0x%08x link=%{public}s configROM=0x%08x "
             "selfID=%{public}s async=%{public}s",
             ohciVersion, linkEnabled ? "enabled" : "disabled", configRomMap, selfIdState,
             asyncState);
}

} // namespace

namespace ASFW::Driver {

ControllerCore::ControllerCore(ControllerConfig config, RolePolicy initialPolicy, Dependencies deps)
    : config_(std::move(config)),
      rolePolicy_(initialPolicy),
      deps_(std::move(deps)),
      roleCoordinator_(Role::RoleExecutors{
          static_cast<Role::IPhyConfigReset*>(this),
          static_cast<Role::IRemoteCsrWriter*>(this),
          static_cast<Role::IContenderControl*>(this)}) {

    // FW-21: the RoleCoordinator's mutating actions are gated by the capability
    // ladder. Seed it from the initial role policy; ApplyRolePolicy() keeps the
    // gate in sync on any subsequent runtime change.
    roleCoordinator_.SetActivityLevel(rolePolicy_.fullBMActivityLevel);
    roleCoordinator_.SetLinuxStyleCmcForceRoot(rolePolicy_.linuxStyleCmcForceRoot);

    broadcastChannel_ = deps_.broadcastChannel;
    if (broadcastChannel_ && deps_.hardware) {
        localIrmController_ = std::make_unique<Bus::LocalIRMResourceController>(*deps_.hardware, *broadcastChannel_);
        ASFW_LOG(Controller, "✅ LocalIRMResourceController created");
    }

    if (deps_.hardware && deps_.busReset) {
        Bus::IRMFallbackCoordinator::Deps fallbackDeps{
            .hardware = *deps_.hardware,
            .timing = &deps_.busReset->PostResetTiming(),
            .scheduler = deps_.scheduler.get()
        };
        irmFallback_ = std::make_shared<Bus::IRMFallbackCoordinator>(fallbackDeps);
        ASFW_LOG(Controller, "✅ IRMFallbackCoordinator created");
    }

    cyclePolicy_ = std::make_unique<Bus::CyclePolicyCoordinator>();
    ASFW_LOG(Controller, "✅ CyclePolicyCoordinator created");

    rootSelection_ = std::make_unique<Bus::RootSelectionCoordinator>(Bus::RootSelectionConfig{});
    ASFW_LOG(Controller, "✅ RootSelectionCoordinator created");

    gapPolicy_ = std::make_unique<Bus::GapPolicyCoordinator>(Bus::GapPolicyConfig{});
    ASFW_LOG(Controller, "✅ GapPolicyCoordinator created");

    powerLinkPolicy_ = std::make_unique<Bus::PowerLinkPolicyCoordinator>(Bus::PowerLinkPolicyConfig{});
    ASFW_LOG(Controller, "✅ PowerLinkPolicyCoordinator created");

    speedMapService_ = std::make_shared<Bus::SpeedMapService>();
    ASFW_LOG(Controller, "✅ SpeedMapService created");

    if (deps_.asyncController && deps_.topology) {
        busImpl_ =
            std::make_unique<Async::FireWireBusImpl>(*deps_.asyncController, *deps_.topology);
        ASFW_LOG(Controller, "✅ FireWireBusImpl facade created");
        bmPolicyCoordinator_ = std::make_unique<Bus::BusManagerPolicyCoordinator>(
            Bus::BusManagerPolicyCoordinator::Deps{
                .hardware = deps_.hardware.get(),
                .executor = this
            }
        );
        ASFW_LOG(Controller, "✅ BusManagerPolicyCoordinator created");
    }

    if (deps_.hardware && deps_.asyncController) {
        deps_.hardware->BindAsyncControllerPort(deps_.asyncController.get());
        ASFW_LOG(Controller, "✅ HardwareInterface bound to async controller port for PHY packets");
    }

    // Note: DMAMemoryImpl will be instantiated lazily in DMA() accessor
    // since DMAMemoryManager is created during AsyncSubsystem::Start()
}

ControllerCore::~ControllerCore() { Stop(); }

void ControllerCore::LogBuildBanner() const {
    ASFW_LOG(Controller, "═══════════════════════════════════════════════════════════");
    ASFW_LOG(Controller, "%{public}s", Version::kFullVersionString);
    ASFW_LOG(Controller, "%{public}s", Version::kBuildInfoString);
    if (Version::kGitDirty) {
        ASFW_LOG(Controller, "⚠️  DIRTY BUILD: Working tree has uncommitted changes");
    }
    ASFW_LOG(Controller, "Build host: %{public}s", Version::kBuildHost);
    ASFW_LOG(Controller, "═══════════════════════════════════════════════════════════");
}

kern_return_t ControllerCore::InitializeBusResetAndDiscovery() {
    if (!(deps_.busReset && deps_.hardware && deps_.scheduler && deps_.asyncController &&
          deps_.selfId && deps_.configRomStager && deps_.interrupts && deps_.topology)) {
        ASFW_LOG(Controller,
                 "❌ CRITICAL: Missing dependencies for BusResetCoordinator initialization");
        return kIOReturnNoResources;
    }

    ApplyBringupOverrides(config_, deps_.busManager.get());

    auto workQueue = deps_.scheduler->Queue();
    ASFW_LOG(Controller, "Initializing BusResetCoordinator");

    deps_.busReset->Initialize(deps_.hardware.get(), workQueue, deps_.asyncController.get(),
                               deps_.selfId.get(), deps_.configRomStager.get(),
                               deps_.interrupts.get(), deps_.topology.get(), deps_.busManager.get(),
                               deps_.romScanner.get(), deps_.topologyMapService.get());

    ASFW_LOG(Controller, "Binding topology callback for Discovery integration");
    deps_.busReset->BindCallbacks(
        [this](const TopologySnapshot& snap) { this->OnTopologyReady(snap); });

    if (deps_.romScanner && deps_.topology) {
        deps_.romScanner->SetTopologyManager(deps_.topology.get());
    }

    return kIOReturnSuccess;
}

kern_return_t ControllerCore::Start(IOService* provider) {
    if (running_) {
        return kIOReturnSuccess;
    }

    if (deps_.stateMachine) {
        deps_.stateMachine->TransitionTo(ControllerState::kStarting, "ControllerCore::Start",
                                         mach_absolute_time());
    }

    LogBuildBanner();

    const kern_return_t initStatus = InitializeBusResetAndDiscovery();
    if (initStatus != kIOReturnSuccess) {
        return initStatus;
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
            deps_.stateMachine->TransitionTo(ControllerState::kFailed,
                                             "ControllerCore::Start hardware init failed",
                                             mach_absolute_time());
        }
        return kr;
    }

    if (!deps_.interrupts) {
        ASFW_LOG_V0(Controller, "❌ CRITICAL: No InterruptManager - cannot enable interrupts!");
        if (deps_.stateMachine) {
            deps_.stateMachine->TransitionTo(ControllerState::kFailed,
                                             "ControllerCore::Start missing InterruptManager",
                                             mach_absolute_time());
        }
        return kIOReturnNoResources;
    }

    // Arm the controller to receive interrupts only after the Self-ID buffer, Config ROM,
    // and link control bits are staged. This mirrors linux firewire/ohci.c:2470-2586,
    // where IntMaskSet is written immediately before linkEnable.
    running_ = true;
    ASFW_LOG(Controller,
             "Enabling IOInterruptDispatchSource AFTER hardware staging (Linux ordering)...");
    deps_.interrupts->Enable();
    ASFW_LOG(Controller, "✓ IOInterruptDispatchSource enabled");

    kr = EnableInterruptsAndStartBus();
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Controller, "❌ Final enable sequence failed: 0x%08x", kr);
        deps_.interrupts->Disable();
        running_ = false;
        hardwareAttached_ = false;
        if (deps_.stateMachine) {
            deps_.stateMachine->TransitionTo(ControllerState::kFailed,
                                             "ControllerCore::Start enable failed",
                                             mach_absolute_time());
        }
        return kr;
    }

    ASFW_LOG(Controller, "✓ Hardware initialization complete - interrupt delivery active");

    if (deps_.topologyMapService) {
        if (!deps_.topologyMapService->Start()) {
            ASFW_LOG(Controller, "⚠️ WARNING: TopologyMapService failed to start");
        }
    }

    if (deps_.stateMachine) {
        deps_.stateMachine->TransitionTo(ControllerState::kRunning,
                                         "ControllerCore::Start complete", mach_absolute_time());
    }
    return kIOReturnSuccess;
}

void ControllerCore::Stop() {
    if (!running_) {
        return;
    }

    ASFW_LOG(Controller, "ControllerCore::Stop - beginning shutdown sequence");

    if (deps_.stateMachine) {
        deps_.stateMachine->TransitionTo(ControllerState::kQuiescing, "ControllerCore::Stop",
                                         mach_absolute_time());
    }

    // Disable interrupts FIRST to prevent new events during shutdown
    if (deps_.interrupts) {
        ASFW_LOG(Controller, "Disabling IOInterruptDispatchSource...");
        deps_.interrupts->Disable();
        ASFW_LOG(Controller, "✓ Interrupts disabled");
    }

    // Mark as not running to prevent HandleInterrupt from processing events
    running_ = false;

    if (deps_.topologyMapService) {
        deps_.topologyMapService->Stop();
    }

    if (deps_.busManagerElectionDriver) {
        deps_.busManagerElectionDriver->Stop();
    }

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
        deps_.stateMachine->TransitionTo(ControllerState::kStopped, "ControllerCore::Stop complete",
                                         mach_absolute_time());
    }

    ASFW_LOG(Controller, "✓ ControllerCore::Stop complete");
}

kern_return_t ControllerCore::PerformSoftReset() const {
    if (!deps_.hardware) {
        ASFW_LOG(Hardware, "No hardware interface for software reset");
        return kIOReturnNoDevice;
    }

    auto& hw = *deps_.hardware;
    ASFW_LOG(Hardware, "Performing software reset...");
    hw.SetHCControlBits(HCControlBits::kSoftReset);

    using ASFW::Driver::kSoftResetPollUsec;
    using ASFW::Driver::kSoftResetTimeoutUsec;

    // Wait for softReset bit to CLEAR (hardware clears it when reset complete)
    const bool cleared =
        hw.WaitHC(HCControlBits::kSoftReset, false, kSoftResetTimeoutUsec, kSoftResetPollUsec);
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
    const kern_return_t lpsStatus = EnableLinkPowerStatus(hw);
    if (lpsStatus != kIOReturnSuccess) {
        return lpsStatus;
    }

    if (broadcastChannel_) {
        broadcastChannel_->ResetImplementedInvalid();
    }

    // Step 3: Detect OHCI version
    const uint32_t version = hw.Read(Register32::kVersion);
    ohciVersion_ = version & 0x00FF00FF; // Store for feature detection
    const bool isOHCI_1_1_OrLater = (ohciVersion_ >= ASFW::Driver::kOHCI_1_1);

    // Step 3a: Enable OHCI 1.1+ features if supported
    // OHCI 1.1 spec §5.5: Program initial default values for autonomous IRM CSRs.
    // This prepares the controller to host IRM resources correctly after a bus reset.
    if (isOHCI_1_1_OrLater) {
        const kern_return_t kr = hw.ProgramInitialIRMResourceRegisters();
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Hardware, "❌ Failed to program initial IRM registers: 0x%08x", kr);
            // We continue anyway, as basic operation might still work
        }
    }

    // Step 4: Clear noByteSwapData - enable byte-swapping for data phases per OHCI spec
    // Per OHCI §5.7: noByteSwapData=0 enables endianness conversion for packet data
    // macOS is little-endian, most FireWire devices expect big-endian wire format
    hw.ClearHCControlBits(HCControlBits::kNoByteSwap);

    // Step 5: Check if PHY register programming is allowed
    // Per OHCI §5.7.2: programPhyEnable bit indicates if generic software can configure PHY
    const uint32_t hcControlBefore = hw.ReadHCControl();
    const bool programPhyEnableSupported =
        (hcControlBefore & HCControlBits::kProgramPhyEnable) != 0;
    phyProgramSupported_ = programPhyEnableSupported;

    ASFW_LOG(Hardware, "HCControl=0x%08x (programPhyEnable=%{public}s)", hcControlBefore,
             programPhyEnableSupported ? "YES" : "NO");

    if (!programPhyEnableSupported) {
        ASFW_LOG(Hardware,
                 "WARNING: programPhyEnable=0 - PHY may be pre-configured by firmware/BIOS");
        ASFW_LOG(Hardware, "Per OHCI §5.7.2: Generic software may not modify PHY configuration");
        ASFW_LOG(Hardware,
                 "Skipping PHY register 4 configuration (PHY should already be configured)");
        // Don't fail - firmware may have already configured PHY correctly
    }

    bool phyConfigOk = false;
    if (programPhyEnableSupported) {
        phyConfigOk = ConfigurePhyRegisters(hw, config_, rolePolicy_);
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
    FinalizePhyLinkConfiguration(hw, programPhyEnableSupported, phyConfigOk);

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
    // TODO(ASFW-DMA): Confirm whether remote DMA still requires this register programming.
    // Per Linux ohci_enable(): Don't pre-write NodeID; bus reset will assign it from Self-ID
    // The kProvisionalNodeId value would be immediately overwritten anyway
    hw.SetLinkControlBits(ASFW::Driver::kDefaultLinkControl);
    ASFW_LOG(Hardware,
             "LinkControl: rcvSelfID | rcvPhyPkt | cycleTimerEnable "
             "(cycleMaster is role-policy controlled)");
    hw.WriteAndFlush(Register32::kAsReqFilterHiSet, ASFW::Driver::kAsReqAcceptAllMask);

    ConfigureAtRetries(hw);

    // Bus timing state: mark cycle timer as inactive during init
    // Linux: ohci->bus_time_running = false;
    // Ensures init path doesn't assume active isochronous timing
    busTimeRunning_ = false;
    ASFW_LOG(Hardware, "Bus time marked inactive - isochronous cycle timer not yet running");

    ClearIsoReceiveMultiChannelMode(hw);
    return PrepareSelfIdBuffer(deps_.selfId, hw);
}

kern_return_t ControllerCore::EnableInterruptsAndStartBus() {
    if (hardwareInitialised_) {
        return kIOReturnSuccess;
    }
    if (!deps_.hardware) {
        ASFW_LOG(Hardware, "EnableInterruptsAndStartBus: no hardware interface");
        return kIOReturnNoDevice;
    }

    auto& hw = *deps_.hardware;
    SeedInitialInterruptMask(hw, deps_.interrupts.get());

    ASFW_LOG(Hardware,
             "Setting linkEnable + BIBimageValid atomically - will trigger auto bus reset");
    hw.SetHCControlBits(HCControlBits::kLinkEnable | HCControlBits::kBibImageValid);
    MaybeForceInitialBusReset(hw, phyProgramSupported_, phyConfigOk_);

    const kern_return_t armStatus = ArmAsyncReceiveContexts(deps_.asyncController.get());
    if (armStatus != kIOReturnSuccess) {
        return armStatus;
    }

    hardwareInitialised_ = true;
    LogInitSummary(hw, ohciVersion_, deps_.selfId, deps_.asyncController);
    return kIOReturnSuccess;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ControllerCore::StageConfigROM(uint32_t busOptions, uint32_t guidHi,
                                             uint32_t guidLo) const {
    if (!deps_.configRom || !deps_.configRomStager || !deps_.hardware) {
        ASFW_LOG(Hardware, "Config ROM dependencies missing (builder=%p stager=%p hw=%p)",
                 deps_.configRom.get(), deps_.configRomStager.get(), deps_.hardware.get());
        return kIOReturnNotReady;
    }

    auto builder = deps_.configRom;
    const uint64_t hardwareGuid =
        (static_cast<uint64_t>(guidHi) << 32) | static_cast<uint64_t>(guidLo);
    const uint64_t effectiveGuid = (config_.localGuid != 0) ? config_.localGuid : hardwareGuid;

    // FW-11/FW-22: advertise only the capabilities ASFW actually backs, gated by
    // the configured role mode. In FullBusManager validation mode, normalize the
    // local BIB like an OHCI host instead of trusting a zeroed hardware capability
    // nibble; peers use this evidence when deciding who can safely be root.
    const uint32_t localBusOptions = ASFW::FW::NormalizeLocalBusOptions(
        busOptions, rolePolicy_.roleMode, rolePolicy_.fullBMActivityLevel);
    const auto advertisedCaps = ASFW::FW::DecodeBusOptions(localBusOptions);
    ASFW_LOG(Hardware,
             "FW-22: roleMode=%u advertising bmc=%d irmc=%d cmc=%d isc=%d (hw=0x%08x -> 0x%08x)",
             static_cast<unsigned>(rolePolicy_.roleMode), advertisedCaps.bmc ? 1 : 0,
             advertisedCaps.irmc ? 1 : 0, advertisedCaps.cmc ? 1 : 0, advertisedCaps.isc ? 1 : 0,
             busOptions, localBusOptions);
    builder->Build(localBusOptions, effectiveGuid, ASFW::Driver::MakeNodeCapabilities(phyConfigOk_),
                   config_.vendor.vendorName);
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

kern_return_t ControllerCore::ApplyRolePolicy(const RolePolicy& policy) {
    rolePolicy_ = policy;
    // Keep the RoleCoordinator gate in sync with the new policy.
    roleCoordinator_.SetActivityLevel(rolePolicy_.fullBMActivityLevel);
    roleCoordinator_.SetLinuxStyleCmcForceRoot(rolePolicy_.linuxStyleCmcForceRoot);

    if (deps_.busManagerElectionDriver) {
        deps_.busManagerElectionDriver->SetRolePolicy(policy);
    }

    // Before the link is up there is nothing to re-advertise — Start() stages the
    // Config ROM from rolePolicy_ during bring-up. Once running, re-stage the BIB
    // capabilities and force a long bus reset so peers re-read the local ROM.
    if (!running_ || !deps_.hardware) {
        return kIOReturnSuccess;
    }

    auto& hw = *deps_.hardware;
    const uint32_t busOptions = hw.Read(Register32::kBusOptions);
    const uint32_t guidHi = hw.Read(Register32::kGUIDHi);
    const uint32_t guidLo = hw.Read(Register32::kGUIDLo);
    const kern_return_t kr = StageConfigROM(busOptions, guidHi, guidLo);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Controller, "ApplyRolePolicy: Config ROM re-stage failed: 0x%08x", kr);
        return kr;
    }
    ASFW_LOG(Controller,
             "ApplyRolePolicy: roleMode=%u activity=%u — re-staged BIB, forcing bus reset",
             static_cast<unsigned>(rolePolicy_.roleMode),
             static_cast<unsigned>(rolePolicy_.fullBMActivityLevel));
    hw.InitiateBusReset(/*shortReset=*/false);
    return kIOReturnSuccess;
}

void ControllerCore::DiagnoseUnrecoverableError() const {
    if (!deps_.hardware) {
        return;
    }

    auto& hw = *deps_.hardware;

    struct ContextInfo {
        const char* shortName;
        uint32_t controlSetReg;
    };

    const ContextInfo contexts[] = {
        {.shortName = "ATreq", .controlSetReg = DMAContextHelpers::AsReqTrContextControlSet},
        {.shortName = "ATrsp", .controlSetReg = DMAContextHelpers::AsRspTrContextControlSet},
        {.shortName = "ARreq", .controlSetReg = DMAContextHelpers::AsReqRcvContextControlSet},
        {.shortName = "ARrsp", .controlSetReg = DMAContextHelpers::AsRspRcvContextControlSet},
    };

    std::string contextSummary;
    contextSummary.reserve(64);

    bool anyDead = false;
    for (const auto& ctx : contexts) {
        const uint32_t control = hw.Read(Register32FromOffsetUnchecked(ctx.controlSetReg));
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
            std::snprintf(buf, sizeof(buf), "DEAD(0x%02x:%s)", eventCode, codeName);
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
             "UnrecoverableError contexts: %{public}s HCControl=0x%08x(BIB=%d link=%d) "
             "SelfIDBuffer=0x%08x SelfIDCount=0x%08x",
             contextSummary.c_str(), hcControl, bibValid, linkEnable, selfIDBufferReg,
             selfIDCountReg);

    if (!bibValid) {
        ASFW_LOG(Controller, "  BIBimageValid cleared: Config ROM fetch failure suspected");
    }

    if (selfIDBufferReg == 0) {
        ASFW_LOG(Controller, "  Self-ID buffer register is zero (not armed)");
    }
}

void ControllerCore::HandleCycle64Seconds() {
    // Per Apple's AppleFWOHCI::handleCycle64Int():
    // The OHCI IsochronousCycleTimer register has a 7-bit seconds field that wraps every 128
    // seconds. This interrupt fires every 64 seconds (when bit 6 toggles), allowing us to extend
    // the 7-bit counter to a full 32-bit bus cycle time for accurate long-duration isochronous
    // timing.
    //
    // Algorithm:
    // 1. Read current 7-bit seconds from cycle timer (bits 31:25)
    // 2. If current seconds < low 7 bits of our extended counter, a wrap occurred (add 128)
    // 3. Update extended counter: preserve high bits, replace low 7 bits with current seconds
    //
    // This gives us a monotonically increasing 32-bit bus time counter that never wraps
    // (unless running for ~136 years at 1 second increments).

    if (!deps_.hardware) {
        return;
    }

    const uint32_t cycleTimer = deps_.hardware->ReadCycleTime();
    uint32_t seconds = cycleTimer >> 25; // Extract 7-bit seconds field

    // Compare with low 7 bits of our extended counter
    const uint32_t prevLow7 = busCycleTime_ & 0x7F;
    if (seconds < prevLow7) {
        // Wrap-around occurred: seconds went from 127 -> 0
        seconds += 128;
    }

    // Update extended bus cycle time: keep high bits, add current seconds delta
    busCycleTime_ = (busCycleTime_ & 0xFFFFFF80) + seconds;

    ASFW_LOG_V2(Controller, "Cycle64Seconds: timer=0x%08x sec=%u busCycleTime_=%u", cycleTimer,
                seconds, busCycleTime_);
}

} // namespace ASFW::Driver
