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

// kImmediate on every command: one ORB_POINTER write per command. Both
// references drive SCSI targets exactly this way — Linux sbp2_send_orb
// (sbp2.c:1486, doorbell never written) and Apple's SCSI transport
// (IOFireWireSerialBusProtocolTransport.cpp:944 sets kFWSBP2CommandImmediate
// unconditionally; IOFireWireSBP2Login's doorbell/appendORB path is only
// reachable from the userspace SBP2Lib API). The doorbell+next_ORB chain
// (v39–v41) deviated from both references and wedged the LS-9000 after ~92
// commands (2026-07-04 log: ORB fetched, status never posted; AGENT_RESET
// ACKed but execution stayed dead).
uint32_t BuildCommandFlags(SCSI::DataDirection direction) {
    uint32_t flags = SBP2CommandORB::kNotify | SBP2CommandORB::kNormalORB |
                     SBP2CommandORB::kImmediate;
    if (direction == SCSI::DataDirection::FromTarget) {
        flags |= SBP2CommandORB::kDataFromTarget;
    }
    return flags;
}

constexpr uint32_t kManagementTimeoutGraceMs = 1000;

} // namespace

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
    if (commandInFlight_ && resultCallback_) {
        FailActiveCommand(static_cast<int>(kIOReturnAborted),
                          Wire::SBPStatus::kRequestAborted);
    }
    CleanupCommandResources();
    CleanupManagementResources();
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
    if (session_.State() != LoginState::LoggedIn || commandInFlight_) {
        return false;
    }
    if (request.transferLength > 0 && request.direction == SCSI::DataDirection::None) {
        return false;
    }
    if (request.direction == SCSI::DataDirection::FromTarget && !request.outgoingPayload.empty()) {
        return false;
    }
    if (request.direction == SCSI::DataDirection::ToTarget &&
        request.outgoingPayload.size() != request.transferLength) {
        return false;
    }

    const uint16_t maxCDB = session_.TargetInfo().maxCommandBlockSize;
    if (maxCDB < request.cdb.size()) {
        return false;
    }

    uint64_t bufferHandle = 0;
    AddressSpaceManager::AddressRangeMeta bufferMeta{};
    if (request.transferLength > 0) {
        const kern_return_t kr = addrSpaceMgr_.AllocateAddressRangeAuto(
            owner_, 0xFFFF, request.transferLength, &bufferHandle, &bufferMeta);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Async, "CommandExecutor: failed to allocate command buffer: 0x%08x", kr);
            return false;
        }
    }

    std::unique_ptr<SBP2PageTable> pageTable;
    if (request.transferLength > 0) {
        if (request.direction == SCSI::DataDirection::ToTarget) {
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
        if (!commandInFlight_ || commandORB_.get() != submittedORB) {
            return;
        }

        commandInFlight_ = false;
        commandReady_ = true;
        lastCompletedCommandOpcode_ = activeCommandOpcode_;
        activeCommandOpcode_.reset();

        SCSI::CommandResult result{};
        result.transportStatus = transportStatus;
        result.sbpStatus = sbpStatus;

        if (transportStatus == 0 && sbpStatus == Wire::SBPStatus::kNoAdditionalInfo &&
            activeCommandRequest_.has_value() &&
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
            // Target saw this ORB — keep it as the doorbell-chain anchor.
            RetireCommandKeepAnchor();
            NotifyResultCallback();
            return;
        }
        // Transport failure → agent gets reset; no chain to anchor.
        CleanupCommandResources();
        // ORB timeout means the target may be wedged beyond what AGENT_RESET
        // revives (fetches ORBs, never executes). Apple's transport resets the
        // LUN before completing the task
        // (IOFireWireSerialBusProtocolTransport.cpp:1554), so the initiator's
        // retry runs against a freshly reset execution engine. Delivery of the
        // failed result is deferred until the LUN reset completes.
        if (SubmitTaskManagement(SBP2ManagementORB::Function::LogicalUnitReset,
                                 [this]() {
                                     // lastError_ carries the management ORB outcome (set by
                                     // the mgmt completion before onComplete). Nonzero means
                                     // the target never fetched the LUN-reset ORB — its fetch
                                     // engine is dead and no management function can reach it.
                                     // Apple's transport stops here and just fails the task
                                     // (IOFireWireSerialBusProtocolTransport.cpp:2030 ignores
                                     // the LUN-reset status), leaving the device wedged until
                                     // power cycle; its user-client path instead resets the
                                     // bus (IOFireWireSBP2UserClient.cpp:427). The LS-9000
                                     // wedges exactly this way, so escalate to a bus reset —
                                     // reconnect/re-login runs via the normal reset flow.
                                     if (lastError_ != 0 && busResetRequester_) {
                                         ASFW_LOG(Async,
                                                  "CommandExecutor: LUN reset failed (%d) — "
                                                  "escalating to bus reset", lastError_);
                                         busResetRequester_();
                                     }
                                     NotifyResultCallback();
                                 })) {
            ASFW_LOG(Async, "CommandExecutor: transport failure — LUN reset before completing task");
            return;
        }
        NotifyResultCallback();
    });

    commandInFlight_ = true;
    commandReady_ = false;
    pendingCommandResult_.reset();
    activeCommandRequest_ = request;
    activeCommandOpcode_ = request.cdb.front();
    commandBufferHandle_ = bufferHandle;
    commandORB_ = std::move(orb);
    commandPageTable_ = std::move(pageTable);
    resultCallback_ = std::move(callback);

    // Bring-up trace: identifies the wedging command if the target stops
    // posting status mid-sequence.
    ASFW_LOG(Async, "CommandExecutor: submit opcode=0x%02x dir=%d xfer=%u timeout=%ums",
             request.cdb.front(), static_cast<int>(request.direction),
             request.transferLength, request.timeoutMs);
    if (session_.SubmitORB(commandORB_.get())) {
        return true;
    }

    // Submission rejected synchronously — roll back. The callback is dropped
    // unfired (the caller sees `false` and handles the rejection itself).
    if (commandORB_.get() == submittedORB) {
        commandInFlight_ = false;
        commandReady_ = false;
        pendingCommandResult_.reset();
        activeCommandRequest_.reset();
        activeCommandOpcode_.reset();
        resultCallback_ = {};
        CleanupCommandResources();
    }
    return false;
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
    SCSI::CommandResult result = std::move(*pendingCommandResult_);
    pendingCommandResult_.reset();
    lastCompletedCommandOpcode_.reset();
    commandReady_ = false;
    return result;
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
    // Grace beyond the advertised management timeout: the LS-9000 posted its
    // LUN-reset status 2 ms past its 2000 ms Unit_Characteristics timeout
    // (v45 HW trace) and lost the race. Apple tolerates a late status because
    // its status FIFO outlives the timeout; ours is torn down with the ORB, so
    // give the target headroom instead.
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
        if (status == 0 && IsSupportedTaskManagementFunction(function)) {
            // A push-mode command wiped by task management must still complete
            // (the bridge holds an open SCSI task on it).
            if (commandInFlight_ && resultCallback_) {
                FailActiveCommand(static_cast<int>(kIOReturnAborted),
                                  Wire::SBPStatus::kRequestAborted);
            }
            CleanupCommandResources();
            activeCommandRequest_.reset();
            activeCommandOpcode_.reset();
            // Don't wipe a pending result awaiting deferred delivery (the
            // timeout→LUN-reset escalation delivers it via onComplete below).
            if (!resultCallback_) {
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
    CleanupManagementResources();
    if (commandInFlight_ || commandORB_) {
        FailActiveCommand(static_cast<int>(kIOReturnAborted), Wire::SBPStatus::kRequestAborted);
    }
}

void CommandExecutor::Cleanup() {
    // Release-time teardown: a pending push-mode command must complete (aborted)
    // so its task is not leaked. May run under the registry lock — the bridge's
    // callback defers any registry re-entry.
    if (commandInFlight_ && resultCallback_) {
        FailActiveCommand(static_cast<int>(kIOReturnAborted),
                          Wire::SBPStatus::kRequestAborted);
    }
    CleanupCommandResources();
    CleanupManagementResources();
}

void CommandExecutor::FailActiveCommand(int transportStatus, uint8_t sbpStatus) noexcept {
    if (!commandInFlight_) {
        return;
    }

    commandInFlight_ = false;
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
    commandPageTable_.reset();
    if (commandBufferHandle_ != 0) {
        addrSpaceMgr_.DeallocateAddressRange(owner_, commandBufferHandle_);
        commandBufferHandle_ = 0;
    }
    if (commandORB_) {
        commandORB_->SetAppended(false);
    }

    CleanupCommandResources();
    NotifyResultCallback();
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
    if (result.has_value()) {
        cb(*result);
    }
}

void CommandExecutor::RetireCommandKeepAnchor() {
    // Normal completion: free the data buffer and page table, but keep the
    // completed ORB alive as the chain anchor — FetchAgent links the NEXT
    // command into its next_ORB field and rings the doorbell, and the target
    // re-reads that field from our memory (Apple keeps fLastORB the same way).
    // The previous anchor is safe to drop now: its next_ORB link was consumed
    // when this command was fetched.
    if (commandBufferHandle_ != 0) {
        addrSpaceMgr_.DeallocateAddressRange(owner_, commandBufferHandle_);
        commandBufferHandle_ = 0;
    }
    linkAnchorORB_ = std::move(commandORB_);
    commandPageTable_.reset();
    commandInFlight_ = false;
}

void CommandExecutor::CleanupCommandResources() {
    // Mirror #19: wipe the session's fetch-agent ORB tracking (cancels any
    // in-flight fetch-agent/doorbell write) before dropping our ORB resources.
    session_.ClearCommandTracking();
    if (commandBufferHandle_ != 0) {
        addrSpaceMgr_.DeallocateAddressRange(owner_, commandBufferHandle_);
        commandBufferHandle_ = 0;
    }
    commandORB_.reset();
    linkAnchorORB_.reset();
    commandPageTable_.reset();
    commandInFlight_ = false;
}

void CommandExecutor::CleanupManagementResources() {
    managementORB_.reset();
}

} // namespace ASFW::Protocols::SBP2
