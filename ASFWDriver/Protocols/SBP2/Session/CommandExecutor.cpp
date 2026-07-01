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
    uint32_t flags = SBP2CommandORB::kNotify | SBP2CommandORB::kImmediate |
                     SBP2CommandORB::kNormalORB;
    if (direction == SCSI::DataDirection::FromTarget) {
        flags |= SBP2CommandORB::kDataFromTarget;
    }
    return flags;
}

} // namespace

CommandExecutor::CommandExecutor(Async::IFireWireBus& bus,
                                 Async::IFireWireBusInfo& busInfo,
                                 AddressSpaceManager& addrSpaceMgr,
                                 LoginSession& session,
                                 void* owner,
                                 int32_t& lastError,
                                 IODispatchQueue* workQueue) noexcept
    : bus_(bus)
    , busInfo_(busInfo)
    , addrSpaceMgr_(addrSpaceMgr)
    , session_(session)
    , owner_(owner)
    , lastError_(lastError)
    , workQueue_(workQueue) {}

CommandExecutor::~CommandExecutor() {
    lifetimeToken_.reset();
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

bool CommandExecutor::SubmitCommand(const SCSI::CommandRequest& request) {
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
        pendingCommandResult_ = std::move(result);
        activeCommandRequest_.reset();
        CleanupCommandResources();
    });

    commandInFlight_ = true;
    commandReady_ = false;
    pendingCommandResult_.reset();
    activeCommandRequest_ = request;
    activeCommandOpcode_ = request.cdb.front();
    commandBufferHandle_ = bufferHandle;
    commandORB_ = std::move(orb);
    commandPageTable_ = std::move(pageTable);

    if (session_.SubmitORB(commandORB_.get())) {
        return true;
    }

    // Submission rejected synchronously — roll back.
    if (commandORB_.get() == submittedORB) {
        commandInFlight_ = false;
        commandReady_ = false;
        pendingCommandResult_.reset();
        activeCommandRequest_.reset();
        activeCommandOpcode_.reset();
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

bool CommandExecutor::SubmitTaskManagement(SBP2ManagementORB::Function function) {
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
    orb->SetTimeout(session_.TargetInfo().managementTimeoutMs);
    orb->SetWorkQueue(workQueue_);
    orb->SetTargetNode(session_.Generation(), session_.TargetInfo().targetNodeId);

    const SBP2ManagementORB* submittedORB = orb.get();
    const std::weak_ptr<int> weak = lifetimeToken_;
    orb->SetCompletionCallback([this, weak, submittedORB, function](int status) {
        if (weak.expired()) {
            return;
        }
        if (managementORB_.get() != submittedORB) {
            return;
        }
        lastError_ = static_cast<int32_t>(status);
        if (status == 0 && IsSupportedTaskManagementFunction(function)) {
            CleanupCommandResources();
            pendingCommandResult_.reset();
            activeCommandRequest_.reset();
            activeCommandOpcode_.reset();
            lastCompletedCommandOpcode_.reset();
            commandReady_ = false;
        }
        managementORB_.reset();
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
    commandPageTable_.reset();
    commandInFlight_ = false;
}

void CommandExecutor::CleanupManagementResources() {
    managementORB_.reset();
}

} // namespace ASFW::Protocols::SBP2
