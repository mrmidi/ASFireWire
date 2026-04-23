#include "SBP2SessionRegistry.hpp"

#include "../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Discovery/FWUnit.hpp"

#include <cstring>

namespace ASFW::Protocols::SBP2 {

namespace {

class IOLockGuard {
public:
    explicit IOLockGuard(IOLock* lock) : lock_(lock) {
        if (lock_ != nullptr) {
            IOLockLock(lock_);
        }
    }

    ~IOLockGuard() {
        if (lock_ != nullptr) {
            IOLockUnlock(lock_);
        }
    }

    IOLockGuard(const IOLockGuard&) = delete;
    IOLockGuard& operator=(const IOLockGuard&) = delete;

private:
    IOLock* lock_{nullptr};
};

constexpr uint8_t kInquiryOpcode = 0x12;

uint32_t BuildCommandFlags(SCSI::DataDirection direction) {
    uint32_t flags = SBP2CommandORB::kNotify |
        SBP2CommandORB::kImmediate |
        SBP2CommandORB::kNormalORB;
    if (direction == SCSI::DataDirection::FromTarget) {
        flags |= SBP2CommandORB::kDataFromTarget;
    }
    return flags;
}

SBP2TargetInfo BuildTargetInfoFromUnit(const Discovery::FWUnit& unit) {
    SBP2TargetInfo info{};

    info.managementAgentOffset = unit.GetManagementAgentOffset().value_or(0);
    info.lun = static_cast<uint16_t>(unit.GetLUN().value_or(0) & 0xFFFF);

    if (auto uc = unit.GetUnitCharacteristics(); uc.has_value()) {
        const uint32_t value = *uc;
        const uint8_t orbSizeUnits = static_cast<uint8_t>((value >> 24) & 0xFF);
        const uint8_t timeoutUnits = static_cast<uint8_t>((value >> 16) & 0xFF);
        info.managementTimeoutMs = static_cast<uint32_t>(timeoutUnits) * 500;
        info.maxORBSize = std::max<uint16_t>(static_cast<uint16_t>(orbSizeUnits) * 4, 32);
    }
    info.maxCommandBlockSize = info.maxORBSize > Wire::NormalORB::kHeaderSize
        ? static_cast<uint16_t>(info.maxORBSize - Wire::NormalORB::kHeaderSize)
        : 0;

    if (auto fastStart = unit.GetFastStart(); fastStart.has_value()) {
        const uint32_t value = *fastStart;
        info.fastStartSupported = true;
        info.fastStartOffset = static_cast<uint8_t>((value >> 8) & 0xFF);
        info.fastStartMaxPayload = static_cast<uint8_t>(value & 0xFF);
    }

    if (auto device = unit.GetDevice(); device) {
        info.targetNodeId = device->GetNodeID();
    }

    return info;
}

} // namespace

SBP2SessionRegistry::SBP2SessionRegistry(Async::IFireWireBus& bus,
                                         Async::IFireWireBusInfo& busInfo,
                                         AddressSpaceManager& addrSpaceMgr,
                                         Discovery::IDeviceManager& deviceManager,
                                         IODispatchQueue* workQueue)
    : bus_(bus)
    , busInfo_(busInfo)
    , addrSpaceMgr_(addrSpaceMgr)
    , deviceManager_(deviceManager)
    , workQueue_(workQueue) {
    lock_ = IOLockAlloc();
}

SBP2SessionRegistry::~SBP2SessionRegistry() {
    IOLockGuard lock(lock_);
    for (auto& [handle, record] : sessions_) {
        if (record.session && record.session->State() == LoginState::LoggedIn) {
            (void)record.session->Logout();
        }
        CleanupCommandResources(record);
    }
    sessions_.clear();

    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

std::expected<uint64_t, int> SBP2SessionRegistry::CreateSession(void* owner,
                                                                uint64_t guid,
                                                                uint32_t romOffset) {
    auto unit = ResolveUnit(guid, romOffset);
    if (!unit) {
        ASFW_LOG(SBP2, "SBP2SessionRegistry: no unit found for guid=0x%016llx romOffset=%u",
                 guid, romOffset);
        return std::unexpected(kIOReturnNotFound);
    }

    if (!unit->Matches(kSBP2UnitSpecId, kSBP2UnitSwVersion)) {
        ASFW_LOG(SBP2,
                 "SBP2SessionRegistry: unit identity spec=0x%06x sw=0x%06x is not SBP-2",
                 unit->GetUnitSpecID(), unit->GetUnitSwVersion());
        return std::unexpected(kIOReturnUnsupported);
    }

    const auto mgmtOffset = unit->GetManagementAgentOffset();
    if (!mgmtOffset.has_value() || *mgmtOffset == 0) {
        ASFW_LOG(SBP2, "SBP2SessionRegistry: unit has no Management_Agent_Offset");
        return std::unexpected(kIOReturnUnsupported);
    }

    auto targetInfo = BuildTargetInfoFromUnit(*unit);
    if (targetInfo.managementAgentOffset == 0) {
        return std::unexpected(kIOReturnUnsupported);
    }

    auto session = std::make_unique<SBP2LoginSession>(bus_, busInfo_, addrSpaceMgr_);
    session->Configure(targetInfo);
    session->SetWorkQueue(workQueue_);

    IOLockGuard lock(lock_);
    const uint64_t handle = nextHandle_++;

    SBP2SessionRecord record{};
    record.handle = handle;
    record.owner = owner;
    record.guid = guid;
    record.romOffset = romOffset;
    record.session = std::move(session);

    auto [it, inserted] = sessions_.emplace(handle, std::move(record));
    if (!inserted) {
        return std::unexpected(kIOReturnNoMemory);
    }

    ASFW_LOG(SBP2, "SBP2SessionRegistry: created session handle=%llu guid=0x%016llx romOffset=%u",
             handle, guid, romOffset);
    return handle;
}

bool SBP2SessionRegistry::StartLogin(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->session) {
        return false;
    }

    if (record->session->State() != LoginState::Idle) {
        return false;
    }

    record->session->SetLoginCallback([this, handle](const LoginCompleteParams& params) {
        IOLockGuard cbLock(lock_);
        auto* rec = FindByHandle(handle);
        if (rec == nullptr) {
            return;
        }

        rec->state.lastError = params.status;
        if (params.status == 0) {
            rec->state.loginID = params.loginResponse.loginID;
            rec->state.loginState = LoginState::LoggedIn;
            rec->state.generation = params.generation;
        } else {
            rec->state.loginState = LoginState::Failed;
        }
    });

    return record->session->Login();
}

std::optional<SBP2SessionState> SBP2SessionRegistry::GetSessionState(uint64_t handle) const {
    IOLockGuard lock(lock_);
    const auto* record = FindByHandle(handle);
    if (!record || !record->session) {
        return std::nullopt;
    }

    SBP2SessionState state{};
    state.loginState = record->session->State();
    state.loginID = record->session->LoginID();
    state.generation = record->session->Generation();
    state.lastError = record->state.lastError;
    state.reconnectPending = (state.loginState == LoginState::Suspended);
    return state;
}

bool SBP2SessionRegistry::SubmitInquiry(uint64_t handle, uint8_t allocationLength) {
    return SubmitCommand(handle, SCSI::BuildInquiryRequest(allocationLength));
}

std::optional<std::vector<uint8_t>> SBP2SessionRegistry::GetInquiryResult(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->commandReady || !record->pendingCommandResult.has_value() ||
        record->lastCompletedCommandOpcode != kInquiryOpcode) {
        return std::nullopt;
    }

    if (record->pendingCommandResult->transportStatus != 0 ||
        record->pendingCommandResult->sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        record->pendingCommandResult.reset();
        record->lastCompletedCommandOpcode.reset();
        record->commandReady = false;
        return std::nullopt;
    }

    auto payload = std::move(record->pendingCommandResult->payload);
    record->pendingCommandResult.reset();
    record->lastCompletedCommandOpcode.reset();
    record->commandReady = false;
    return payload;
}

bool SBP2SessionRegistry::SubmitCommand(uint64_t handle, const SCSI::CommandRequest& request) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->session || request.cdb.empty()) {
        return false;
    }

    if (record->session->State() != LoginState::LoggedIn || record->commandInFlight) {
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

    const uint16_t maxCDB = record->session->TargetInfo().maxCommandBlockSize;
    if (maxCDB < request.cdb.size()) {
        return false;
    }

    uint64_t bufferHandle = 0;
    AddressSpaceManager::AddressRangeMeta bufferMeta{};
    if (request.transferLength > 0) {
        const kern_return_t kr = addrSpaceMgr_.AllocateAddressRangeAuto(
            record->owner, 0xFFFF, request.transferLength, &bufferHandle, &bufferMeta);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(SBP2, "SBP2SessionRegistry: failed to allocate command buffer: 0x%08x", kr);
            return false;
        }
    }

    std::unique_ptr<SBP2PageTable> pageTable;
    if (request.transferLength > 0) {
        if (request.direction == SCSI::DataDirection::ToTarget) {
            const kern_return_t writeKr = addrSpaceMgr_.WriteLocalData(
                record->owner,
                bufferHandle,
                0,
                std::span<const uint8_t>{request.outgoingPayload.data(), request.outgoingPayload.size()});
            if (writeKr != kIOReturnSuccess) {
                addrSpaceMgr_.DeallocateAddressRange(record->owner, bufferHandle);
                return false;
            }
        }

        pageTable = std::make_unique<SBP2PageTable>(addrSpaceMgr_, record->owner);
        SBP2PageTable::Segment segment{bufferMeta.address, request.transferLength};
        if (!pageTable->Build(std::span<const SBP2PageTable::Segment>(&segment, 1),
                              busInfo_.GetLocalNodeID().value)) {
            addrSpaceMgr_.DeallocateAddressRange(record->owner, bufferHandle);
            return false;
        }
    }

    auto orb = std::make_unique<SBP2CommandORB>(addrSpaceMgr_, record->owner, maxCDB);
    orb->SetCommandBlock(std::span<const uint8_t>{request.cdb.data(), request.cdb.size()});
    orb->SetFlags(BuildCommandFlags(request.direction));
    orb->SetMaxPayloadSize(record->session->MaxPayloadSize());
    orb->SetTimeout(request.timeoutMs > 0
        ? request.timeoutMs
        : record->session->TargetInfo().managementTimeoutMs);
    if (pageTable) {
        orb->SetDataDescriptor(pageTable->GetResult());
    }

    const uint64_t captureHandle = handle;
    orb->SetCompletionCallback([this, captureHandle](int transportStatus, uint8_t sbpStatus) {
        IOLockGuard cbLock(lock_);
        auto* rec = FindByHandle(captureHandle);
        if (rec == nullptr) {
            return;
        }

        rec->commandInFlight = false;
        rec->commandReady = true;
        rec->lastCompletedCommandOpcode = rec->activeCommandOpcode;
        rec->activeCommandOpcode.reset();

        SCSI::CommandResult result{};
        result.transportStatus = transportStatus;
        result.sbpStatus = sbpStatus;

        if (transportStatus == 0 &&
            sbpStatus == Wire::SBPStatus::kNoAdditionalInfo &&
            rec->activeCommandRequest.has_value() &&
            rec->activeCommandRequest->direction == SCSI::DataDirection::FromTarget &&
            rec->activeCommandRequest->transferLength > 0 &&
            rec->commandBufferHandle != 0) {
            std::vector<uint8_t> payload;
            const kern_return_t readKr = addrSpaceMgr_.ReadIncomingData(
                rec->owner,
                rec->commandBufferHandle,
                0,
                rec->activeCommandRequest->transferLength,
                &payload);
            if (readKr == kIOReturnSuccess) {
                result.payload = std::move(payload);
            } else {
                result.transportStatus = static_cast<int>(readKr);
            }
        }

        if (rec->activeCommandRequest.has_value() && rec->activeCommandRequest->captureSenseData) {
            result.senseData = result.payload;
        }

        rec->state.lastError = static_cast<int32_t>(result.transportStatus);
        if (result.transportStatus == 0 && result.sbpStatus == Wire::SBPStatus::kNoAdditionalInfo) {
            rec->state.lastError = 0;
        }
        rec->pendingCommandResult = std::move(result);
        rec->activeCommandRequest.reset();
        CleanupCommandResources(*rec);
    });

    if (!record->session->SubmitORB(orb.get())) {
        if (bufferHandle != 0) {
            addrSpaceMgr_.DeallocateAddressRange(record->owner, bufferHandle);
        }
        return false;
    }

    record->commandInFlight = true;
    record->commandReady = false;
    record->pendingCommandResult.reset();
    record->activeCommandRequest = request;
    record->activeCommandOpcode = request.cdb.front();
    record->commandBufferHandle = bufferHandle;
    record->commandORB = std::move(orb);
    record->commandPageTable = std::move(pageTable);
    return true;
}

std::optional<SCSI::CommandResult> SBP2SessionRegistry::GetCommandResult(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->commandReady || !record->pendingCommandResult.has_value()) {
        return std::nullopt;
    }

    SCSI::CommandResult result = std::move(*record->pendingCommandResult);
    record->pendingCommandResult.reset();
    record->lastCompletedCommandOpcode.reset();
    record->commandReady = false;
    return result;
}

bool SBP2SessionRegistry::ReleaseSession(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto it = sessions_.find(handle);
    if (it == sessions_.end()) {
        return false;
    }

    auto& record = it->second;
    if (record.session && record.session->State() == LoginState::LoggedIn) {
        (void)record.session->Logout();
    }

    CleanupCommandResources(record);
    sessions_.erase(it);
    return true;
}

void SBP2SessionRegistry::ReleaseOwner(void* owner) {
    IOLockGuard lock(lock_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.owner == owner) {
            auto& record = it->second;
            if (record.session && record.session->State() == LoginState::LoggedIn) {
                (void)record.session->Logout();
            }
            CleanupCommandResources(record);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void SBP2SessionRegistry::OnBusReset(uint16_t newGeneration) {
    IOLockGuard lock(lock_);
    for (auto& [handle, record] : sessions_) {
        if (record.session) {
            record.session->HandleBusReset(newGeneration);
        }
        if (record.commandInFlight || record.commandORB) {
            record.commandInFlight = false;
            record.commandReady = true;
            record.lastCompletedCommandOpcode = record.activeCommandOpcode;
            record.activeCommandOpcode.reset();

            SCSI::CommandResult result{};
            result.transportStatus = static_cast<int>(kIOReturnAborted);
            result.sbpStatus = Wire::SBPStatus::kRequestAborted;
            record.pendingCommandResult = std::move(result);
            record.state.lastError = static_cast<int32_t>(kIOReturnAborted);
            record.activeCommandRequest.reset();
            CleanupCommandResources(record);
        }
    }
}

void SBP2SessionRegistry::RefreshTargets(Discovery::Generation gen) {
    IOLockGuard lock(lock_);
    for (auto& [handle, record] : sessions_) {
        if (!record.session || record.session->State() != LoginState::Suspended) {
            continue;
        }

        auto unit = ResolveUnit(record.guid, record.romOffset);
        if (!unit) {
            ASFW_LOG(SBP2, "SBP2SessionRegistry: RefreshTargets: unit not found for handle=%llu",
                     handle);
            continue;
        }

        auto targetInfo = BuildTargetInfoFromUnit(*unit);
        record.session->Configure(targetInfo);

        ASFW_LOG(SBP2, "SBP2SessionRegistry: reconnecting session handle=%llu gen=%u",
                 handle, gen.value);
        (void)record.session->Reconnect();
    }
}

SBP2SessionRecord* SBP2SessionRegistry::FindByHandle(uint64_t handle) {
    auto it = sessions_.find(handle);
    return it != sessions_.end() ? &it->second : nullptr;
}

const SBP2SessionRecord* SBP2SessionRegistry::FindByHandle(uint64_t handle) const {
    auto it = sessions_.find(handle);
    return it != sessions_.end() ? &it->second : nullptr;
}

std::shared_ptr<Discovery::FWUnit> SBP2SessionRegistry::ResolveUnit(uint64_t guid,
                                                                    uint32_t romOffset) const {
    const auto devices = deviceManager_.GetAllDevices();
    for (const auto& device : devices) {
        if (!device || device->GetGUID() != guid) {
            continue;
        }
        for (const auto& unit : device->GetUnits()) {
            if (unit && unit->GetDirectoryOffset() == romOffset) {
                return unit;
            }
        }
    }
    return nullptr;
}

void SBP2SessionRegistry::CleanupCommandResources(SBP2SessionRecord& record) {
    if (record.session) {
        record.session->ClearORBTracking(true);
    }
    if (record.commandBufferHandle != 0) {
        addrSpaceMgr_.DeallocateAddressRange(record.owner, record.commandBufferHandle);
        record.commandBufferHandle = 0;
    }
    record.commandORB.reset();
    record.commandPageTable.reset();
    record.commandInFlight = false;
}

} // namespace ASFW::Protocols::SBP2
