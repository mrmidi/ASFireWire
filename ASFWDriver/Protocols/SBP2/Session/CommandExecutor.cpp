// CommandExecutor — SBP-2 command plane for one session. See CommandExecutor.hpp.
//
// Ported from PR #19's SBP2SessionRegistry command methods (§2b). The registry
// owns one CommandExecutor per SessionRecord and holds its lock around the
// synchronous entry points; async ORB/management completions run on the single
// Default queue and are guarded by the weak lifetime token.

#include "CommandExecutor.hpp"

#include "../../../Async/Interfaces/IFireWireBus.hpp"
#include "../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../../Common/FWCommon.hpp"

#include <span>
#include <utility>
#include <vector>

namespace ASFW::Protocols::SBP2 {

namespace {

constexpr uint8_t kInquiryOpcode = 0x12;

uint32_t BuildCommandFlags(SCSI::DataDirection direction) {
    uint32_t flags = SBP2CommandORB::kNotify | SBP2CommandORB::kNormalORB;
    if (direction == SCSI::DataDirection::FromTarget) {
        flags |= SBP2CommandORB::kDataFromTarget;
    }
    return flags;
}

constexpr uint32_t kManagementTimeoutGraceMs = 1000;

} // namespace

ParkedCommand::ParkedCommand(AddressSpaceManager& addrSpaceMgr,
                             void* owner,
                             uint64_t bufferHandle,
                             std::unique_ptr<SBP2CommandORB> orb,
                             std::unique_ptr<SBP2PageTable> pageTable) noexcept
    : addrSpaceMgr_(&addrSpaceMgr)
    , owner_(owner)
    , bufferHandle_(bufferHandle)
    , orb_(std::move(orb))
    , pageTable_(std::move(pageTable)) {}

ParkedCommand::ParkedCommand(ParkedCommand&& other) noexcept
    : addrSpaceMgr_(other.addrSpaceMgr_)
    , owner_(other.owner_)
    , bufferHandle_(std::exchange(other.bufferHandle_, 0))
    , orb_(std::move(other.orb_))
    , pageTable_(std::move(other.pageTable_)) {}

ParkedCommand& ParkedCommand::operator=(ParkedCommand&& other) noexcept {
    if (this != &other) {
        Release();
        addrSpaceMgr_ = other.addrSpaceMgr_;
        owner_ = other.owner_;
        bufferHandle_ = std::exchange(other.bufferHandle_, 0);
        orb_ = std::move(other.orb_);
        pageTable_ = std::move(other.pageTable_);
    }
    return *this;
}

ParkedCommand::~ParkedCommand() {
    Release();
}

void ParkedCommand::Release() noexcept {
    orb_.reset();
    pageTable_.reset();
    if (bufferHandle_ != 0) {
        addrSpaceMgr_->DeallocateAddressRange(owner_, std::exchange(bufferHandle_, 0));
    }
}

CommandExecutor::CommandExecutor(Async::IFireWireBus& bus,
                                 Async::IFireWireBusInfo& busInfo,
                                 AddressSpaceManager& addrSpaceMgr,
                                 LoginSession& session,
                                 ISessionScheduler& scheduler,
                                 void* owner,
                                 int32_t& lastError) noexcept
    : bus_(bus)
    , busInfo_(busInfo)
    , addrSpaceMgr_(addrSpaceMgr)
    , session_(session)
    , scheduler_(scheduler)
    , owner_(owner)
    , lastError_(lastError) {}

CommandExecutor::~CommandExecutor() {
    lifetimeToken_.reset();
    if (phase_ == CommandPhase::Active && resultCallback_) {
        FailActiveCommand(static_cast<int>(kIOReturnAborted),
                          Wire::SBPStatus::kRequestAborted);
    }
    CleanupCommandResources();
    CleanupManagementResources();
    // Flush a result deferred behind a (now-dropped) management ORB; see
    // OnBusReset. Idempotent — a no-op if FailActiveCommand already fired.
    if (resultCallback_) {
        NotifyResultCallback();
    }
}

bool CommandExecutor::IsSupportedTaskManagementFunction(
    SBP2ManagementORB::Function function) noexcept {
    switch (function) {
        case SBP2ManagementORB::Function::AbortTaskSet:
        case SBP2ManagementORB::Function::LogicalUnitReset:
        case SBP2ManagementORB::Function::TargetReset:
            return true;
        default:
            return false;
    }
}

bool CommandExecutor::SubmitInquiry(uint8_t allocationLength) {
    return SubmitCommand(SCSI::BuildInquiryRequest(allocationLength));
}

bool CommandExecutor::SubmitCommand(const SCSI::CommandRequest& request,
                                    ResultCallback callback) {
    if (request.cdb.empty()) {
        return false;
    }
    if (session_.State() != LoginState::LoggedIn || phase_ != CommandPhase::Idle) {
        return false;
    }
    if (request.transferLength > 0 && request.direction == SCSI::DataDirection::None) {
        return false;
    }
    if (request.direction == SCSI::DataDirection::FromTarget && !request.outgoingPayload.empty()) {
        return false;
    }
    if (request.direction == SCSI::DataDirection::ToTarget && request.dataIOVA == 0 &&
        request.outgoingPayload.size() != request.transferLength) {
        return false;
    }

    const uint16_t maxCDB = session_.TargetInfo().maxCommandBlockSize;
    if (maxCDB < request.cdb.size()) {
        return false;
    }

    // The target moves the data itself through the OHCI physical response unit;
    // LoginSession admitted its node when the login completed. A caller that
    // owns a DMA mapping (the HBA's task buffer) names it directly; a caller
    // without one (the user client) gets a bounce buffer in the same space.
    uint64_t bufferHandle = 0;
    AddressSpaceManager::AddressRangeMeta bufferMeta{};
    const bool inPlace = request.dataIOVA != 0;
    if (inPlace) {
        bufferMeta.address = request.dataIOVA;
    } else if (request.transferLength > 0) {
        const kern_return_t kr = addrSpaceMgr_.AllocatePhysicalRange(
            owner_, request.transferLength, &bufferHandle, &bufferMeta);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Async, "CommandExecutor: failed to allocate command buffer: 0x%08x", kr);
            return false;
        }
    }

    std::unique_ptr<SBP2PageTable> pageTable;
    if (request.transferLength > 0) {
        if (!inPlace && request.direction == SCSI::DataDirection::ToTarget) {
            const kern_return_t writeKr = addrSpaceMgr_.WriteLocalData(
                owner_, bufferHandle, 0,
                std::span<const uint8_t>{request.outgoingPayload.data(),
                                         request.outgoingPayload.size()});
            if (writeKr != kIOReturnSuccess) {
                addrSpaceMgr_.DeallocateAddressRange(owner_, bufferHandle);
                return false;
            }
        }

        pageTable = std::make_unique<SBP2PageTable>(addrSpaceMgr_, owner_);
        SBP2PageTable::Segment segment{bufferMeta.address, request.transferLength};
        if (!pageTable->Build(std::span<const SBP2PageTable::Segment>(&segment, 1),
                              busInfo_.GetLocalNodeID().value)) {
            addrSpaceMgr_.DeallocateAddressRange(owner_, bufferHandle);
            return false;
        }
    }

    auto orb = std::make_unique<SBP2CommandORB>(addrSpaceMgr_, owner_, maxCDB);
    if (!orb->SetCommandBlock(std::span<const uint8_t>{request.cdb.data(), request.cdb.size()})) {
        if (bufferHandle != 0) {
            addrSpaceMgr_.DeallocateAddressRange(owner_, bufferHandle);
        }
        return false;
    }
    orb->SetFlags(BuildCommandFlags(request.direction));
    orb->SetTimeout(request.timeoutMs > 0 ? request.timeoutMs
                                          : session_.TargetInfo().managementTimeoutMs);
    if (pageTable) {
        orb->SetDataDescriptor(pageTable->GetResult());
    }

    const SBP2CommandORB* submittedORB = orb.get();
    const std::weak_ptr<int> weak = lifetimeToken_;
    orb->SetCompletionCallback([this, weak, submittedORB](int transportStatus, uint8_t sbpStatus) {
        if (weak.expired()) {
            return;
        }
        if (phase_ != CommandPhase::Active || commandORB_.get() != submittedORB) {
            return;
        }

        commandReady_ = true;
        lastCompletedCommandOpcode_ = activeCommandOpcode_;
        activeCommandOpcode_.reset();

        SCSI::CommandResult result{};
        result.transportStatus = transportStatus;
        result.sbpStatus = sbpStatus;

        const bool completed = transportStatus == 0 &&
                               sbpStatus == Wire::SBPStatus::kNoAdditionalInfo &&
                               activeCommandRequest_.has_value();
        if (completed && activeCommandRequest_->dataIOVA != 0) {
            // SBP-2 status carries no residual; the whole transfer is reported.
            result.inPlaceBytes = activeCommandRequest_->transferLength;
        } else if (completed &&
            activeCommandRequest_->direction == SCSI::DataDirection::FromTarget &&
            activeCommandRequest_->transferLength > 0 && commandBufferHandle_ != 0) {
            std::vector<uint8_t> payload;
            const kern_return_t readKr = addrSpaceMgr_.ReadIncomingData(
                owner_, commandBufferHandle_, 0,
                activeCommandRequest_->transferLength, &payload);
            if (readKr == kIOReturnSuccess) {
                result.payload = std::move(payload);
            } else {
                result.transportStatus = static_cast<int>(readKr);
            }
        }

        if (activeCommandRequest_.has_value() && activeCommandRequest_->captureSenseData) {
            result.senseData = result.payload;
        }

        // Surface SCSI status + autosense from the status block's
        // command-set-dependent bytes (stashed on the ORB by
        // FetchAgent::OnStatusBlock). Without this a CHECK CONDITION is
        // invisible: sbpStatus stays 0 and the command looks GOOD with zero
        // data.
        if (commandORB_) {
            const auto statusData = commandORB_->CompletionStatusData();
            if (!statusData.empty()) {
                result.scsiStatus = statusData[0] & 0x3F;
                result.scsiStatusValid = true;
                if (result.scsiStatus == 0x02) {  // CHECK CONDITION → decode autosense
                    auto sense = SCSI::ConvertSBP2StatusToSenseData(statusData);
                    if (!sense.empty()) {
                        result.senseData = std::move(sense);
                    }
                }
            }
        }

        lastError_ = (result.transportStatus == 0 &&
                      result.sbpStatus == Wire::SBPStatus::kNoAdditionalInfo)
                         ? 0
                         : static_cast<int32_t>(result.transportStatus);
        const bool delivered = (result.transportStatus == 0);
        pendingCommandResult_ = std::move(result);
        activeCommandRequest_.reset();
        if (delivered) {
            RetireCommand();
            NotifyResultCallback();
            return;
        }
        // Transport failure: no status block, so the target may still be
        // moving data into the buffer. SBP-2 §10.4 reuses the memory only once
        // a task-management status says the target stopped, so the ORB, page
        // table and buffer stay mapped through the LUN reset. Apple's transport
        // resets the LUN before completing the task
        // (IOFireWireSerialBusProtocolTransport.cpp:1554); delivery of the
        // failed result waits for that too.
        phase_ = CommandPhase::Recovering;
        session_.ClearCommandTracking();
        recoveryResetInFlight_ = true;
        if (SubmitTaskManagement(SBP2ManagementORB::Function::LogicalUnitReset,
                                 [this]() {
                                     recoveryResetInFlight_ = false;
                                     // lastError_ carries the management ORB
                                     // outcome. Nonzero means the target never
                                     // fetched the LUN-reset ORB — its fetch
                                     // engine is dead and no management
                                     // function can reach it. Apple's user
                                     // client resets the bus for that
                                     // (IOFireWireSBP2UserClient.cpp:427), and
                                     // only the reset ends the target's claim
                                     // on the buffer: it stays mapped until
                                     // OnBusReset.
                                     if (lastError_ != 0) {
                                         if (busResetRequester_) {
                                             ASFW_LOG(Async,
                                                      "CommandExecutor: LUN reset failed (%d) — "
                                                      "escalating to bus reset", lastError_);
                                             busResetRequester_();
                                         }
                                         // A caller-owned buffer is reused once
                                         // the result is delivered, so an
                                         // in-place result waits for the reset
                                         // (OnBusReset flushes it); a bounce
                                         // buffer is ours to keep.
                                         if (!commandInPlace_) {
                                             NotifyResultCallback();
                                         }
                                         return;
                                     }
                                     // LUN reset status: every task is gone and
                                     // the buffer is ours again (§10.4.3).
                                     CleanupCommandResources();
                                     // A target may leave its fetch agent dead
                                     // after a LUN reset (status dead bit, seen
                                     // on a Hasselblad H camera). Apple resets
                                     // the agent from the LUN-reset completion
                                     // and completes the task only after that
                                     // (IOFireWireSerialBusProtocolTransport.cpp
                                     // LunResetComplete). AGENT_RESET has no
                                     // status (§6.4.2); its outcome changes
                                     // nothing here.
                                     const std::weak_ptr<int> weakSelf = lifetimeToken_;
                                     session_.ResetFetchAgentNoWait([this, weakSelf]() {
                                         if (weakSelf.expired()) {
                                             return;
                                         }
                                         NotifyResultCallback();
                                     });
                                 })) {
            ASFW_LOG(Async, "CommandExecutor: transport failure — LUN reset before completing task");
            return;
        }
        recoveryResetInFlight_ = false;
        // No LUN reset could be issued: the session is no longer LoggedIn (a
        // reset cleared the filter and OnBusReset owns the rest) or a client
        // task-management ORB is in flight, whose status is the stop point. A
        // caller-owned buffer waits for that status (the management completion
        // below frees and delivers); a bounce buffer is ours, so match Linux's
        // abort (sbp2.c:1550-1551): release and complete.
        if (commandInPlace_ && session_.State() == LoginState::LoggedIn) {
            ASFW_LOG(Async, "CommandExecutor: transport failure, holding in-place result "
                            "for the task-management status");
            return;
        }
        CleanupCommandResources();
        NotifyResultCallback();
    });

    phase_ = CommandPhase::Active;
    commandReady_ = false;
    pendingCommandResult_.reset();
    activeCommandRequest_ = request;
    activeCommandOpcode_ = request.cdb.front();
    commandBufferHandle_ = bufferHandle;
    commandInPlace_ = inPlace;
    commandORB_ = std::move(orb);
    commandPageTable_ = std::move(pageTable);
    resultCallback_ = std::move(callback);

    if (session_.SubmitORB(commandORB_.get())) {
        return true;
    }

    // SubmitORB returned false. Two distinct cases, told apart by whether the
    // ORB completion callback ran synchronously during the call — it leaves
    // Active when it does.
    if (phase_ == CommandPhase::Active && commandORB_.get() == submittedORB) {
        // Completion did NOT run: a genuine synchronous rejection (e.g. the
        // session rejected on state before touching the fetch agent). Roll back
        // and report false; the callback is dropped unfired, so the caller
        // completes the task itself, exactly once.
        commandReady_ = false;
        pendingCommandResult_.reset();
        activeCommandRequest_.reset();
        activeCommandOpcode_.reset();
        resultCallback_ = {};
        CleanupCommandResources();
        return false;
    }

    // FetchAgent::AppendImmediate fired the ORB completion synchronously
    // (WriteBlock failed on submit). The result was already delivered, or
    // deferred behind a LUN-reset ORB — either way the executor now owns
    // resultCallback_. Report success so the caller does NOT fire its own
    // fallback completion: doing so would complete the SCSI task twice (double
    // ParallelTaskCompletion + OSAction/controller over-release).
    return true;
}

std::optional<SCSI::CommandResult> CommandExecutor::GetCommandResult() {
    if (!commandReady_ || !pendingCommandResult_.has_value()) {
        return std::nullopt;
    }
    SCSI::CommandResult result = std::move(*pendingCommandResult_);
    pendingCommandResult_.reset();
    lastCompletedCommandOpcode_.reset();
    commandReady_ = false;
    return result;
}

std::optional<SCSI::CommandResult> CommandExecutor::GetInquiryResult() {
    if (!commandReady_ || !pendingCommandResult_.has_value() ||
        lastCompletedCommandOpcode_ != kInquiryOpcode) {
        return std::nullopt;
    }
    return GetCommandResult();
}

bool CommandExecutor::SubmitTaskManagement(SBP2ManagementORB::Function function,
                                           std::function<void()> onComplete) {
    if (!IsSupportedTaskManagementFunction(function)) {
        return false;
    }
    if (session_.State() != LoginState::LoggedIn || managementORB_) {
        return false;
    }

    auto orb = std::make_unique<SBP2ManagementORB>(bus_, busInfo_, addrSpaceMgr_, owner_);
    orb->SetFunction(function);
    orb->SetLoginID(session_.LoginID());
    orb->SetManagementAgentOffset(session_.TargetInfo().managementAgentOffset);
    // Grace beyond the advertised management timeout: a target can post status
    // a hair past its own Unit_Characteristics timeout (observed on the LS-9000,
    // which posted LUN-reset status 2 ms past its 2000 ms limit in a v45 HW trace,
    // and lost the race). Apple tolerates a late status because its status FIFO
    // outlives the timeout; ours is torn down with the ORB, so give any target
    // headroom instead. Not a model-specific value.
    orb->SetTimeout(session_.TargetInfo().managementTimeoutMs + kManagementTimeoutGraceMs);
    orb->SetScheduler(&scheduler_);
    orb->SetTargetNode(session_.Generation(), session_.TargetInfo().targetNodeId);

    const SBP2ManagementORB* submittedORB = orb.get();
    const std::weak_ptr<int> weak = lifetimeToken_;
    orb->SetCompletionCallback(
        [this, weak, submittedORB, function, onComplete = std::move(onComplete)](int status) {
        if (weak.expired()) {
            return;
        }
        if (managementORB_.get() != submittedORB) {
            if (onComplete) {
                onComplete();
            }
            return;
        }
        lastError_ = static_cast<int32_t>(status);
        const bool ownsCommand = phase_ == CommandPhase::Active ||
            (phase_ == CommandPhase::Recovering && !recoveryResetInFlight_);
        if (status == 0 && IsSupportedTaskManagementFunction(function) && ownsCommand) {
            // A push-mode command wiped by task management must still complete
            // (the bridge holds an open SCSI task on it); a result held back by
            // the timeout path goes out now. The status says the target dropped
            // the task set, so its buffer is free (§10.4).
            const bool pushMode = static_cast<bool>(resultCallback_);
            if (pushMode) {
                FailActiveCommand(static_cast<int>(kIOReturnAborted),
                                  Wire::SBPStatus::kRequestAborted);
            }
            CleanupCommandResources();
            activeCommandRequest_.reset();
            activeCommandOpcode_.reset();
            if (pushMode) {
                NotifyResultCallback();
            } else {
                pendingCommandResult_.reset();
                lastCompletedCommandOpcode_.reset();
                commandReady_ = false;
            }
        }
        managementORB_.reset();
        if (onComplete) {
            onComplete();
        }
    });

    managementORB_ = std::move(orb);
    if (!managementORB_->Execute()) {
        managementORB_.reset();
        return false;
    }
    return true;
}

void CommandExecutor::OnBusReset() {
    // The reset ends the target's claim on everything (its node ID and the
    // physical filter are gone with the generation), as it does for Linux
    // (sbp2.c complete_command_orb: unmap on a NULL status).
    CleanupManagementResources();
    if (phase_ == CommandPhase::Active) {
        FailActiveCommand(static_cast<int>(kIOReturnAborted), Wire::SBPStatus::kRequestAborted);
    }
    CleanupCommandResources();
    // A result deferred behind a management ORB (the timeout→LUN-reset
    // escalation) left Active before deferring delivery to that ORB's
    // onComplete. CleanupManagementResources above dropped the ORB without
    // firing onComplete, so the guard misses it — flush any still-armed result
    // now. NotifyResultCallback moves the callback out, so this is a no-op when
    // FailActiveCommand already fired it.
    if (resultCallback_) {
        NotifyResultCallback();
    }
}

std::optional<ParkedCommand> CommandExecutor::Cleanup() {
    // Release-time teardown: a pending push-mode command must complete (aborted)
    // so its task is not leaked. May run under the registry lock — the bridge's
    // callback defers any registry re-entry.
    if (phase_ == CommandPhase::Active && resultCallback_) {
        FailActiveCommand(static_cast<int>(kIOReturnAborted),
                          Wire::SBPStatus::kRequestAborted);
    }
    // The target still owns the mapping until its logout status (§8.4 aborts
    // the task set): hand the resources to whoever waits for that instead of
    // unmapping under a running transfer.
    std::optional<ParkedCommand> parked;
    if (phase_ != CommandPhase::Idle) {
        session_.ClearCommandTracking();
        parked.emplace(addrSpaceMgr_, owner_, std::exchange(commandBufferHandle_, 0),
                       std::move(commandORB_), std::move(commandPageTable_));
        phase_ = CommandPhase::Idle;
    }
    CleanupCommandResources();
    CleanupManagementResources();
    // Flush a result deferred behind a (now-dropped) management ORB; see
    // OnBusReset. Idempotent — a no-op if FailActiveCommand already fired.
    if (resultCallback_) {
        NotifyResultCallback();
    }
    return parked;
}

void CommandExecutor::FailActiveCommand(int transportStatus, uint8_t sbpStatus) noexcept {
    if (phase_ != CommandPhase::Active) {
        return;
    }

    commandReady_ = true;
    lastCompletedCommandOpcode_ = activeCommandOpcode_;
    activeCommandOpcode_.reset();

    SCSI::CommandResult result{};
    result.transportStatus = transportStatus;
    result.sbpStatus = sbpStatus;
    lastError_ = (transportStatus == 0 && sbpStatus == Wire::SBPStatus::kNoAdditionalInfo)
                     ? 0
                     : static_cast<int32_t>(result.transportStatus);
    pendingCommandResult_ = std::move(result);
    activeCommandRequest_.reset();
    if (commandORB_) {
        commandORB_->SetAppended(false);
    }
    // Resources and phase are left to the caller: it frees or parks them
    // first, then delivers the result, so a callback that submits the next
    // command inline finds the executor Idle.
}

void CommandExecutor::NotifyResultCallback() {
    if (!resultCallback_) {
        return;
    }
    // Move out before invoking: the callback may immediately submit the next
    // command, which installs a new resultCallback_.
    ResultCallback cb = std::move(resultCallback_);
    resultCallback_ = {};
    auto result = GetCommandResult();
    // Exactly-once contract: fire even if the result was already consumed, so
    // the caller's retained resources (OSAction/service across the async gap)
    // always release. Fall back to an aborted result rather than dropping cb.
    cb(result.value_or(SCSI::CommandResult{.transportStatus = kIOReturnAborted}));
}

void CommandExecutor::RetireCommand() {
    // Normal completion: the target has posted status and is done with the ORB
    // and the buffer (one ORB_POINTER write per command — no chain to keep
    // alive). Free the data buffer, page table and ORB.
    if (commandBufferHandle_ != 0) {
        addrSpaceMgr_.DeallocateAddressRange(owner_, commandBufferHandle_);
        commandBufferHandle_ = 0;
    }
    commandORB_.reset();
    commandPageTable_.reset();
    phase_ = CommandPhase::Idle;
}

void CommandExecutor::CleanupCommandResources() {
    // Mirror #19: wipe the session's fetch-agent ORB tracking (cancels any
    // in-flight fetch-agent write) before dropping our ORB resources.
    session_.ClearCommandTracking();
    if (commandBufferHandle_ != 0) {
        addrSpaceMgr_.DeallocateAddressRange(owner_, commandBufferHandle_);
        commandBufferHandle_ = 0;
    }
    commandORB_.reset();
    commandPageTable_.reset();
    phase_ = CommandPhase::Idle;
}

void CommandExecutor::CleanupManagementResources() {
    managementORB_.reset();
}

} // namespace ASFW::Protocols::SBP2
