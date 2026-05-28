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
        const uint8_t orbSizeUnits = static_cast<uint8_t>(value & 0xFF);
        const uint8_t timeoutUnits = static_cast<uint8_t>((value >> 8) & 0xFF);
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
    {
        IOLockGuard lock(lock_);
        for (auto& [handle, record] : sessions_) {
            if (record.session && record.session->State() == LoginState::LoggedIn) {
                (void)record.session->Logout();
            }
            CleanupCommandResources(record);
            CleanupManagementResources(record);
        }
        sessions_.clear();
    }
    retiringSessions_.clear();

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

    IOLockGuard lock(lock_);
    if (HasSessionForTargetLocked(guid, romOffset)) {
        ASFW_LOG(SBP2,
                 "SBP2SessionRegistry: duplicate session for guid=0x%016llx romOffset=%u",
                 guid, romOffset);
        return std::unexpected(kIOReturnExclusiveAccess);
    }

    auto session = std::make_shared<SBP2LoginSession>(bus_, busInfo_, addrSpaceMgr_);
    session->Configure(targetInfo);
    session->SetWorkQueue(workQueue_);

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

std::optional<SCSI::CommandResult> SBP2SessionRegistry::GetInquiryResult(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->commandReady || !record->pendingCommandResult.has_value() ||
        record->lastCompletedCommandOpcode != kInquiryOpcode) {
        return std::nullopt;
    }

    SCSI::CommandResult result = std::move(*record->pendingCommandResult);
    record->pendingCommandResult.reset();
    record->lastCompletedCommandOpcode.reset();
    record->commandReady = false;
    return result;
}

bool SBP2SessionRegistry::SubmitCommand(uint64_t handle, const SCSI::CommandRequest& request) {
    std::shared_ptr<SBP2LoginSession> session;
    SBP2CommandORB* submittedORB = nullptr;

    {
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
        if (!orb->SetCommandBlock(std::span<const uint8_t>{request.cdb.data(), request.cdb.size()})) {
            if (bufferHandle != 0) {
                addrSpaceMgr_.DeallocateAddressRange(record->owner, bufferHandle);
            }
            return false;
        }
        orb->SetFlags(BuildCommandFlags(request.direction));
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
            if (!rec->commandInFlight) {
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

        record->commandInFlight = true;
        record->commandReady = false;
        record->pendingCommandResult.reset();
        record->activeCommandRequest = request;
        record->activeCommandOpcode = request.cdb.front();
        record->commandBufferHandle = bufferHandle;
        record->commandORB = std::move(orb);
        record->commandPageTable = std::move(pageTable);
        session = record->session;
        submittedORB = record->commandORB.get();
    }

    if (session->SubmitORB(submittedORB)) {
        return true;
    }

    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (record != nullptr && record->commandORB.get() == submittedORB) {
        record->commandInFlight = false;
        record->commandReady = false;
        record->pendingCommandResult.reset();
        record->activeCommandRequest.reset();
        record->activeCommandOpcode.reset();
        CleanupCommandResources(*record);
    }
    return false;
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

bool SBP2SessionRegistry::SubmitTaskManagement(uint64_t handle,
                                               SBP2ManagementORB::Function function) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->session || !IsSupportedTaskManagementFunction(function)) {
        return false;
    }

    if (record->session->State() != LoginState::LoggedIn || record->managementORB) {
        return false;
    }

    auto orb = std::make_unique<SBP2ManagementORB>(bus_, busInfo_, addrSpaceMgr_, record->owner);
    orb->SetFunction(function);
    orb->SetLoginID(record->session->LoginID());
    orb->SetManagementAgentOffset(record->session->TargetInfo().managementAgentOffset);
    orb->SetTimeout(record->session->TargetInfo().managementTimeoutMs);
    orb->SetWorkQueue(workQueue_);
    orb->SetTimeoutQueue(workQueue_);
    orb->SetTargetNode(record->session->Generation(), record->session->TargetInfo().targetNodeId);

    const auto* submittedORB = orb.get();
    orb->SetCompletionCallback([this, handle, submittedORB, function](int status) {
        IOLockGuard cbLock(lock_);
        auto* rec = FindByHandle(handle);
        if (rec == nullptr || rec->managementORB.get() != submittedORB) {
            return;
        }

        rec->state.lastError = static_cast<int32_t>(status);
        if (status == 0 && IsSupportedTaskManagementFunction(function)) {
            CleanupCommandResources(*rec);
            rec->pendingCommandResult.reset();
            rec->activeCommandRequest.reset();
            rec->activeCommandOpcode.reset();
            rec->lastCompletedCommandOpcode.reset();
            rec->commandReady = false;
        }
        rec->managementORB.reset();
    });

    record->managementORB = std::move(orb);

    if (!record->session->SubmitManagementORB(record->managementORB.get())) {
        record->managementORB.reset();
        return false;
    }

    return true;
}

bool SBP2SessionRegistry::ReleaseSession(uint64_t handle) {
    uint32_t waitMs = 0;

    {
        IOLockGuard lock(lock_);
        auto it = sessions_.find(handle);
        if (it == sessions_.end()) {
            return false;
        }

        auto& record = it->second;
        CleanupCommandResources(record);
        CleanupManagementResources(record);

        if (record.session &&
            (record.session->State() == LoginState::LoggedIn ||
             record.session->State() == LoginState::Suspended)) {
            waitMs = std::max<uint32_t>(record.session->TargetInfo().managementTimeoutMs + 500, 500);
            SetReleaseLogoutCallbackLocked(handle, record.session);
            if (!record.session->Logout()) {
                sessions_.erase(it);
                return true;
            }
        } else if (record.session && record.session->State() == LoginState::LoggingOut) {
            SetReleaseLogoutCallbackLocked(handle, record.session);
            RetireSessionLocked(record);
            sessions_.erase(it);
            return true;
        } else {
            sessions_.erase(it);
            return true;
        }
    }

    for (uint32_t elapsed = 0; elapsed < waitMs; elapsed += 10) {
        IOSleep(10);

        IOLockGuard lock(lock_);
        auto it = sessions_.find(handle);
        if (it == sessions_.end()) {
            return true;
        }

        auto& record = it->second;
        const LoginState state = record.session ? record.session->State() : LoginState::Idle;
        if (state != LoginState::LoggingOut) {
            CleanupCommandResources(record);
            sessions_.erase(it);
            return true;
        }
    }

    IOLockGuard lock(lock_);
    auto it = sessions_.find(handle);
    if (it != sessions_.end()) {
        CleanupCommandResources(it->second);
        CleanupManagementResources(it->second);
        if (it->second.session && it->second.session->State() == LoginState::LoggingOut) {
            RetireSessionLocked(it->second);
        }
        sessions_.erase(it);
    }
    return true;
}

void SBP2SessionRegistry::ReleaseOwner(void* owner) {
    IOLockGuard lock(lock_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.owner == owner) {
            auto& record = it->second;
            if (record.session &&
                (record.session->State() == LoginState::LoggedIn ||
                 record.session->State() == LoginState::Suspended)) {
                SetReleaseLogoutCallbackLocked(record.handle, record.session);
                if (record.session->Logout()) {
                    CleanupCommandResources(record);
                    CleanupManagementResources(record);
                    RetireSessionLocked(record);
                    it = sessions_.erase(it);
                    continue;
                }
            } else if (record.session && record.session->State() == LoginState::LoggingOut) {
                SetReleaseLogoutCallbackLocked(record.handle, record.session);
                CleanupCommandResources(record);
                CleanupManagementResources(record);
                RetireSessionLocked(record);
                it = sessions_.erase(it);
                continue;
            }
            CleanupCommandResources(record);
            CleanupManagementResources(record);
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
        CleanupManagementResources(record);
        if (record.commandInFlight || record.commandORB) {
            FailActiveCommandLocked(record,
                                   static_cast<int>(kIOReturnAborted),
                                   Wire::SBPStatus::kRequestAborted);
        }
    }
}

void SBP2SessionRegistry::FailActiveCommandLocked(SBP2SessionRecord& record,
                                                 int transportStatus,
                                                 uint8_t sbpStatus) noexcept {
    if (!record.commandInFlight) {
        return;
    }

    record.commandInFlight = false;
    record.commandReady = true;
    record.lastCompletedCommandOpcode = record.activeCommandOpcode;
    record.activeCommandOpcode.reset();

    SCSI::CommandResult result{};
    result.transportStatus = transportStatus;
    result.sbpStatus = sbpStatus;
    if (transportStatus == 0 && sbpStatus == Wire::SBPStatus::kNoAdditionalInfo) {
        record.state.lastError = 0;
    } else {
        record.state.lastError = static_cast<int32_t>(result.transportStatus);
    }
    record.pendingCommandResult = std::move(result);
    record.activeCommandRequest.reset();
    record.commandPageTable.reset();
    if (record.commandBufferHandle != 0) {
        addrSpaceMgr_.DeallocateAddressRange(record.owner, record.commandBufferHandle);
        record.commandBufferHandle = 0;
    }
    if (record.commandORB) {
        record.commandORB->SetAppended(false);
    }

    CleanupCommandResources(record);
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

#ifdef ASFW_HOST_TEST
SBP2LoginSession* SBP2SessionRegistry::GetSessionForTesting(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record) {
        return nullptr;
    }
    return record->session.get();
}

std::weak_ptr<SBP2LoginSession> SBP2SessionRegistry::GetSessionWeakForTesting(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record) {
        return {};
    }
    return record->session;
}
#endif

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
        if (!device || !device->IsReady() || device->GetGUID() != guid) {
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

void SBP2SessionRegistry::CleanupManagementResources(SBP2SessionRecord& record) {
    record.managementORB.reset();
}

bool SBP2SessionRegistry::HasSessionForTargetLocked(uint64_t guid, uint32_t romOffset) const {
    for (const auto& [handle, record] : sessions_) {
        if (record.guid == guid && record.romOffset == romOffset && record.session != nullptr) {
            return true;
        }
    }

    return std::any_of(retiringSessions_.begin(), retiringSessions_.end(),
                       [guid, romOffset](const RetiringSession& retired) {
                           return retired.guid == guid &&
                               retired.romOffset == romOffset &&
                               retired.session != nullptr;
                       });
}

void SBP2SessionRegistry::RetireSessionLocked(const SBP2SessionRecord& record) {
    if (record.session == nullptr) {
        return;
    }

    const auto it = std::find_if(
        retiringSessions_.begin(), retiringSessions_.end(),
        [&record](const RetiringSession& retired) {
            return retired.session == record.session;
        });
    if (it ==
        retiringSessions_.end()) {
        retiringSessions_.push_back(RetiringSession{
            .guid = record.guid,
            .romOffset = record.romOffset,
            .session = record.session,
        });
    }
}

void SBP2SessionRegistry::EraseRetiredSessionLocked(
    const std::shared_ptr<SBP2LoginSession>& session) {
    if (session == nullptr) {
        return;
    }

    retiringSessions_.erase(
        std::remove_if(retiringSessions_.begin(), retiringSessions_.end(),
                       [&session](const RetiringSession& retired) {
                           return retired.session == session;
                       }),
        retiringSessions_.end());
}

void SBP2SessionRegistry::SetReleaseLogoutCallbackLocked(
    uint64_t handle,
    const std::shared_ptr<SBP2LoginSession>& session) {
    if (session == nullptr) {
        return;
    }

    std::weak_ptr<SBP2LoginSession> weakSession = session;
    session->SetLogoutCallback([this, handle, weakSession](const LogoutCompleteParams&) {
        IOLockGuard cbLock(lock_);
        std::shared_ptr<SBP2LoginSession> completedSession = weakSession.lock();

        auto it = sessions_.find(handle);
        if (it != sessions_.end() &&
            (completedSession == nullptr || it->second.session == completedSession)) {
            CleanupCommandResources(it->second);
            CleanupManagementResources(it->second);
            sessions_.erase(it);
        }

        EraseRetiredSessionLocked(completedSession);
    });
}

bool SBP2SessionRegistry::IsSupportedTaskManagementFunction(
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

} // namespace ASFW::Protocols::SBP2
