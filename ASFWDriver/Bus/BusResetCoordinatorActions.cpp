#include "BusResetCoordinator.hpp"

#include <cstring>

#include "../Async/Interfaces/IAsyncControllerPort.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "BusManager.hpp"
#include "HardwareInterface.hpp"
#include "InterruptManager.hpp"
#include "Logging.hpp"
#include "SelfIDCapture.hpp"
#include "TopologyManager.hpp"

namespace {

constexpr uint64_t kRepeatedResetHoldoffNs = 2'000'000'000ULL;

void MergePhyConfig(ASFW::Driver::BusManager::PhyConfigCommand& base,
                    const ASFW::Driver::BusManager::PhyConfigCommand& addition) {
    if (addition.gapCount.has_value()) {
        base.gapCount = addition.gapCount;
    }
    if (addition.forceRootNodeID.has_value()) {
        base.forceRootNodeID = addition.forceRootNodeID;
    }
    if (addition.setContender.has_value()) {
        base.setContender = addition.setContender;
    }
}

} // namespace

namespace ASFW::Driver {

void BusResetCoordinator::MaskBusReset() {
    if ((interruptManager_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }

    interruptManager_->MaskInterrupts(hardware_, IntEventBits::kBusReset);
    busResetMasked_ = true;
}

void BusResetCoordinator::UnmaskBusReset() {
    if ((interruptManager_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }

    interruptManager_->UnmaskInterrupts(hardware_, IntEventBits::kBusReset);
    busResetMasked_ = false;
}

void BusResetCoordinator::ForceUnmaskBusResetIfNeeded() {
    if (!busResetMasked_) {
        return;
    }

    if ((interruptManager_ == nullptr) || (hardware_ == nullptr)) {
        ASFW_LOG(BusReset,
                 "busReset remained masked but dependencies are unavailable (irq=%p hw=%p)",
                 interruptManager_, hardware_);
        return;
    }

    interruptManager_->UnmaskInterrupts(hardware_, IntEventBits::kBusReset);
    busResetMasked_ = false;
}

void BusResetCoordinator::ClearStaleSelfIDComplete2() {
    if (hardware_ == nullptr) {
        return;
    }

    // OHCI 1.1 §6.1 / Table 6-1 and §11.5: `selfIDComplete2` retains state
    // across bus resets and is cleared only through `IntEventClear`.
    hardware_->WriteAndFlush(Register32::kIntEventClear, IntEventBits::kSelfIDComplete2);
    selfIdLatch_.stickyComplete = false;
    selfIdLatch_.stickyCompleteTimeNs = 0;
}

void BusResetCoordinator::ClearConsumedSelfIDInterrupts() {
    if (hardware_ == nullptr) {
        selfIdLatch_.Reset();
        return;
    }

    uint32_t clearMask = 0;
    if (selfIdLatch_.complete) {
        clearMask |= IntEventBits::kSelfIDComplete;
    }
    if (selfIdLatch_.stickyComplete) {
        clearMask |= IntEventBits::kSelfIDComplete2;
    }

    if (clearMask != 0U) {
        hardware_->WriteAndFlush(Register32::kIntEventClear, clearMask);
    }

    selfIdLatch_.Reset();
}

void BusResetCoordinator::ArmSelfIDBuffer() {
    if ((selfIdCapture_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }

    if (const kern_return_t kr = selfIdCapture_->Arm(*hardware_); kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(BusReset, "Failed to arm Self-ID buffer: 0x%x", kr);
    }
}

void BusResetCoordinator::StopFlushAT() {
    if (asyncSubsystem_ == nullptr) {
        return;
    }

    const uint8_t nextGeneration =
        static_cast<uint8_t>((lastGeneration_.value + 1U) & 0xFFU);
    asyncSubsystem_->OnBusResetBegin(nextGeneration);
    asyncSubsystem_->StopATContextsOnly();
    asyncSubsystem_->FlushATContexts();
}

bool BusResetCoordinator::DecodeSelfID() {
    if ((selfIdCapture_ == nullptr) || (hardware_ == nullptr)) {
        return false;
    }

    const uint32_t countRegister = hardware_->Read(Register32::kSelfIDCount);
    auto decoded = selfIdCapture_->Decode(countRegister, *hardware_);
    if (!decoded) {
        metrics_.lastFailureReason = std::string{"Self-ID decode failed: "} +
                                     SelfIDCapture::DecodeErrorCodeString(decoded.error().code);
        lastSelfId_.reset();
        ASFW_LOG_V2(BusReset, "Self-ID decode failed: %{public}s",
                    SelfIDCapture::DecodeErrorCodeString(decoded.error().code));
        return false;
    }

    lastSelfId_ = *decoded;
    lastGeneration_ = Discovery::Generation{decoded->generation};
    if (asyncSubsystem_ != nullptr) {
        asyncSubsystem_->ConfirmBusGeneration(static_cast<uint8_t>(decoded->generation & 0xFFU));
    }

    // IEEE 1394-2008 §8.4.5.2 allows corrective PHY configuration before a
    // confirming reset. We keep the more conservative Linux/Apple strategy and
    // rate-limit the follow-up software reset through the coordinator.
    if (decoded->quads.size() > 1U) {
        const uint8_t gapCount = static_cast<uint8_t>((decoded->quads[1] & 0x003F0000U) >> 16U);
        if (gapCount == 0U) {
            ResetDelegationRetryCounter();
            BusManager::PhyConfigCommand command{};
            command.gapCount = std::uint8_t{0x3F};
            RequestSoftwareReset({ResetRequestKind::GapCorrection, ResetFlavor::Long, command,
                                  "Correct zero gap_count"});
        }
    }

    return true;
}

bool BusResetCoordinator::BuildTopology() {
    if ((topologyManager_ == nullptr) || !lastSelfId_.has_value() || (hardware_ == nullptr)) {
        return false;
    }

    const uint32_t nodeIDRegister = hardware_->Read(Register32::kNodeID);
    const uint64_t timestamp = MonotonicNow();

    auto snapshot = topologyManager_->UpdateFromSelfID(*lastSelfId_, timestamp, nodeIDRegister);
    if (!snapshot) {
        metrics_.lastFailureReason = std::string{"Topology build failed: "} +
                                     TopologyManager::TopologyBuildErrorCodeString(
                                         snapshot.error().code);
        lastTopology_.reset();
        RequestSoftwareReset({ResetRequestKind::Recovery, ResetFlavor::Short, std::nullopt,
                              "Invalid Self-ID topology"});
        ASFW_LOG_V2(BusReset, "Topology build failed: %{public}s (%{public}s)",
                    TopologyManager::TopologyBuildErrorCodeString(snapshot.error().code),
                    snapshot.error().detail.c_str());
        return false;
    }

    lastTopology_ = *snapshot;
    resetTiming_.lastSelfIdCompletionNs = timestamp;
    resetTiming_.softwareResetBlockedUntilNs = timestamp + kRepeatedResetHoldoffNs;

    if (!snapshot->gapCountConsistent) {
        ASFW_LOG_V2(BusReset, "Gap counts are inconsistent across validated Self-ID packet 0s");
    }

    return true;
}

void BusResetCoordinator::RestoreConfigROM() {
    if ((configRomStager_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }

    configRomStager_->RestoreHeaderAfterBusReset();
    hardware_->WriteAndFlush(Register32::kBusOptions, configRomStager_->ExpectedBusOptions());
    hardware_->WriteAndFlush(Register32::kConfigROMHeader, configRomStager_->ExpectedHeader());
}

void BusResetCoordinator::ClearBusReset() {
    if (hardware_ == nullptr) {
        return;
    }

    hardware_->WriteAndFlush(Register32::kIntEventClear, IntEventBits::kBusReset);
    busResetClearTime_ = MonotonicNow();
}

void BusResetCoordinator::EnableFilters() {
    if (hardware_ == nullptr) {
        return;
    }

    hardware_->Write(Register32::kAsReqFilterHiSet, kAsReqAcceptAllMask);
    filtersEnabled_ = true;
}

void BusResetCoordinator::RearmAT() {
    if (asyncSubsystem_ == nullptr) {
        return;
    }

    asyncSubsystem_->RearmATContexts();
    atArmed_ = true;
}

void BusResetCoordinator::LogMetrics() {
    const uint64_t completionTime = MonotonicNow();
    metrics_.lastResetStart = firstIrqTime_;
    metrics_.lastResetCompletion = completionTime;

    const double durationMs =
        static_cast<double>(completionTime - firstIrqTime_) / 1'000'000.0;
    ASFW_LOG(BusReset, "Bus reset #%u complete in %.2f ms (generation=%u, aborts=%u)",
             metrics_.resetCount, durationMs, lastGeneration_.value, metrics_.abortCount);

    if (metrics_.lastFailureReason.has_value()) {
        ASFW_LOG_V2(BusReset, "Last failure during recovery: %{public}s",
                    metrics_.lastFailureReason->c_str());
    }
}

void BusResetCoordinator::SendGlobalResumeIfNeeded() {
    if ((hardware_ == nullptr) || !lastTopology_.has_value() ||
        !lastTopology_->localNodeId.has_value()) {
        return;
    }

    const uint32_t generation = lastTopology_->generation;
    if (lastResumeGeneration_ == generation) {
        return;
    }

    const uint8_t localNode = *lastTopology_->localNodeId;
    if (hardware_->SendPhyGlobalResume(localNode)) {
        lastResumeGeneration_ = generation;
    }
}

void BusResetCoordinator::HandleStraySelfID() {
    if (!HasSelfIDCompletion()) {
        return;
    }

    if (!CanAttemptSelfIDDecode()) {
        ClearConsumedSelfIDInterrupts();
        return;
    }

    ASFW_LOG_V2(BusReset, "Handling late Self-ID completion outside active reset flow");
    const bool decoded = DecodeSelfID();
    ClearConsumedSelfIDInterrupts();
    if (decoded) {
        TransitionTo(State::QuiescingAT, "Late Self-ID completion");
    }
}

void BusResetCoordinator::EvaluateRootDelegation(const TopologySnapshot& topology) {
    if (!delegateAttemptActive_) {
        if (delegateSuppressed_ && topology.rootNodeId.has_value() &&
            topology.localNodeId.has_value() &&
            *topology.rootNodeId != *topology.localNodeId) {
            delegateSuppressed_ = false;
        }
        return;
    }

    if (!topology.rootNodeId.has_value()) {
        return;
    }

    const uint8_t currentRoot = *topology.rootNodeId;
    const uint8_t localNode = topology.localNodeId.value_or(0xFF);
    if ((delegateTarget_ != 0xFF && currentRoot == delegateTarget_) ||
        (localNode != 0xFF && currentRoot != localNode)) {
        delegateAttemptActive_ = false;
        delegateSuppressed_ = false;
        delegateTarget_ = 0xFF;
        delegateRetryCount_ = 0;
        return;
    }

    delegateAttemptActive_ = false;
}

void BusResetCoordinator::RequestSoftwareReset(ResetRequest request) {
    const auto strongerFlavor = [](ResetFlavor lhs, ResetFlavor rhs) {
        return (lhs == ResetFlavor::Long || rhs == ResetFlavor::Long) ? ResetFlavor::Long
                                                                      : ResetFlavor::Short;
    };

    if (request.kind == ResetRequestKind::Delegation && delegateSuppressed_) {
        return;
    }

    if (request.kind == ResetRequestKind::Delegation && request.phyConfig.has_value() &&
        request.phyConfig->forceRootNodeID.has_value()) {
        const uint8_t newTarget = *request.phyConfig->forceRootNodeID;
        if (newTarget != delegateTarget_) {
            delegateRetryCount_ = 0;
            delegateTarget_ = newTarget;
        }

        ++delegateRetryCount_;
        if (delegateRetryCount_ > kMaxDelegateRetries) {
            delegateSuppressed_ = true;
            return;
        }

        delegateAttemptActive_ = true;
    }

    if (pendingSoftwareReset_.has_value()) {
        ResetRequest merged = *pendingSoftwareReset_;
        merged.flavor = strongerFlavor(merged.flavor, request.flavor);

        if (merged.phyConfig.has_value() && request.phyConfig.has_value()) {
            auto combined = *merged.phyConfig;
            MergePhyConfig(combined, *request.phyConfig);
            merged.phyConfig = combined;
        } else if (request.phyConfig.has_value()) {
            merged.phyConfig = request.phyConfig;
        }

        if (request.kind == ResetRequestKind::GapCorrection ||
            request.kind == ResetRequestKind::Delegation) {
            merged.kind = request.kind;
        }

        if (!request.reason.empty()) {
            merged.reason = request.reason;
        }

        pendingSoftwareReset_ = std::move(merged);
    } else {
        pendingSoftwareReset_ = std::move(request);
    }

    if ((state_ == State::Idle) && (workQueue_.get() != nullptr)) {
        workQueue_->DispatchAsync(^{
          RunStateMachine();
        });
    }
}

bool BusResetCoordinator::MaybeDispatchPendingSoftwareReset() {
    const auto resetKindString = [](ResetRequestKind kind) {
        switch (kind) {
        case ResetRequestKind::Recovery:
            return "Recovery";
        case ResetRequestKind::GapCorrection:
            return "GapCorrection";
        case ResetRequestKind::Delegation:
            return "Delegation";
        case ResetRequestKind::ManualBusManager:
            return "ManualBusManager";
        }
        return "Unknown";
    };

    const auto resetFlavorString = [](ResetFlavor flavor) {
        return (flavor == ResetFlavor::Short) ? "Short" : "Long";
    };

    if (!pendingSoftwareReset_.has_value() || (hardware_ == nullptr)) {
        return false;
    }

    const uint64_t now = MonotonicNow();
    if ((resetTiming_.softwareResetBlockedUntilNs != 0U) &&
        (now < resetTiming_.softwareResetBlockedUntilNs)) {
        const uint64_t remainingNs = resetTiming_.softwareResetBlockedUntilNs - now;
        const uint32_t remainingMs =
            static_cast<uint32_t>((remainingNs + 999'999ULL) / 1'000'000ULL);
        ASFW_LOG_V2(
            BusReset,
            "Deferring %{public}s %{public}s reset for %u ms per IEEE 1394-2008 §8.2.1",
            resetKindString(pendingSoftwareReset_->kind),
            resetFlavorString(pendingSoftwareReset_->flavor), remainingMs);
        YieldAndReschedule(remainingMs, "Repeated software reset holdoff");
        return true;
    }

    const ResetRequest request = *pendingSoftwareReset_;
    pendingSoftwareReset_.reset();
    return DispatchSoftwareReset(request);
}

bool BusResetCoordinator::DispatchSoftwareReset(const ResetRequest& request) {
    const auto resetKindString = [](ResetRequestKind kind) {
        switch (kind) {
        case ResetRequestKind::Recovery:
            return "Recovery";
        case ResetRequestKind::GapCorrection:
            return "GapCorrection";
        case ResetRequestKind::Delegation:
            return "Delegation";
        case ResetRequestKind::ManualBusManager:
            return "ManualBusManager";
        }
        return "Unknown";
    };

    const auto resetFlavorString = [](ResetFlavor flavor) {
        return (flavor == ResetFlavor::Short) ? "Short" : "Long";
    };

    if (hardware_ == nullptr) {
        return false;
    }

    ASFW_LOG(BusReset, "Issuing %{public}s %{public}s software reset (%{public}s)",
             resetKindString(request.kind), resetFlavorString(request.flavor),
             request.reason.c_str());

    if (request.phyConfig.has_value()) {
        const auto& command = *request.phyConfig;
        if (command.setContender.has_value()) {
            hardware_->SetContender(*command.setContender);
        }

        if (!hardware_->SendPhyConfig(command.gapCount, command.forceRootNodeID,
                                      request.reason.c_str())) {
            if (request.kind == ResetRequestKind::Delegation) {
                delegateAttemptActive_ = false;
                delegateTarget_ = 0xFF;
                delegateRetryCount_ = 0;
                delegateSuppressed_ = false;
            }
            return false;
        }
    }

    if (!hardware_->InitiateBusReset(request.flavor == ResetFlavor::Short)) {
        if (request.kind == ResetRequestKind::Delegation) {
            delegateAttemptActive_ = false;
            delegateTarget_ = 0xFF;
            delegateRetryCount_ = 0;
            delegateSuppressed_ = false;
        }
        return false;
    }

    return true;
}

void BusResetCoordinator::ResetDelegationRetryCounter() {
    delegateRetryCount_ = 0;
    delegateSuppressed_ = false;
}

} // namespace ASFW::Driver
