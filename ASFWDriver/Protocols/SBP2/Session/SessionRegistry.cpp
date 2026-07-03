// SessionRegistry — identity & lifecycle for SBP-2 sessions. See SessionRegistry.hpp.
//
// Ported from PR #19's SBP2SessionRegistry (§2a). The command plane is delegated
// to each record's CommandExecutor (§2b). LoginSession timers run on an injected
// ISessionScheduler; ReleaseSession is async (no IOSleep wait-loop).

#include "SessionRegistry.hpp"

#include "../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../../Discovery/FWDevice.hpp"
#include "../../../Discovery/FWUnit.hpp"

#include <cstring>
#include <utility>

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

SessionRegistry::SessionRegistry(Async::IFireWireBus& bus,
                                 Async::IFireWireBusInfo& busInfo,
                                 AddressSpaceManager& addrSpaceMgr,
                                 Discovery::IDeviceManager& deviceManager,
                                 ISessionScheduler& scheduler,
                                 IODispatchQueue* workQueue)
    : bus_(bus)
    , busInfo_(busInfo)
    , addrSpaceMgr_(addrSpaceMgr)
    , deviceManager_(deviceManager)
    , scheduler_(scheduler)
    , workQueue_(workQueue) {
    lock_ = IOLockAlloc();
}

SessionRegistry::~SessionRegistry() {
    lifetimeToken_.reset();  // fizzle any queued deferred completion bodies
    {
        IOLockGuard lock(lock_);
        for (auto& [handle, record] : sessions_) {
            if (record.session && record.session->State() == LoginState::LoggedIn) {
                (void)record.session->Logout();
            }
            if (record.executor) {
                record.executor->Cleanup();
            }
        }
        sessions_.clear();
    }
    retiringSessions_.clear();

    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

std::expected<uint64_t, int> SessionRegistry::CreateSession(void* owner,
                                                            uint64_t guid,
                                                            uint32_t romOffset) {
    auto unit = ResolveUnit(guid, romOffset);
    if (!unit) {
        ASFW_LOG(Async, "SessionRegistry: no unit found for guid=0x%016llx romOffset=%u",
                 guid, romOffset);
        return std::unexpected(kIOReturnNotFound);
    }

    if (!unit->Matches(kSBP2UnitSpecId, kSBP2UnitSwVersion)) {
        ASFW_LOG(Async, "SessionRegistry: unit identity spec=0x%06x sw=0x%06x is not SBP-2",
                 unit->GetUnitSpecID(), unit->GetUnitSwVersion());
        return std::unexpected(kIOReturnUnsupported);
    }

    const auto mgmtOffset = unit->GetManagementAgentOffset();
    if (!mgmtOffset.has_value() || *mgmtOffset == 0) {
        ASFW_LOG(Async, "SessionRegistry: unit has no Management_Agent_Offset");
        return std::unexpected(kIOReturnUnsupported);
    }

    auto targetInfo = BuildTargetInfoFromUnit(*unit);
    if (targetInfo.managementAgentOffset == 0) {
        return std::unexpected(kIOReturnUnsupported);
    }

    IOLockGuard lock(lock_);
    if (HasSessionForTargetLocked(guid, romOffset)) {
        ASFW_LOG(Async, "SessionRegistry: duplicate session for guid=0x%016llx romOffset=%u",
                 guid, romOffset);
        return std::unexpected(kIOReturnExclusiveAccess);
    }

    const uint64_t handle = nextHandle_++;

    auto [it, inserted] = sessions_.try_emplace(handle);
    if (!inserted) {
        return std::unexpected(kIOReturnNoMemory);
    }

    SessionRecord& record = it->second;
    record.handle = handle;
    record.owner = owner;
    record.guid = guid;
    record.romOffset = romOffset;
    record.session = std::make_shared<LoginSession>(bus_, busInfo_, addrSpaceMgr_, scheduler_);
    record.session->Configure(targetInfo);
    // record.lastError lives at a stable address (map node), so the executor may
    // bind a reference to it.
    record.executor = std::make_unique<CommandExecutor>(
        bus_, busInfo_, addrSpaceMgr_, *record.session, owner, record.lastError, workQueue_);

    ASFW_LOG(Async, "SessionRegistry: created session handle=%llu guid=0x%016llx romOffset=%u",
             handle, guid, romOffset);
    return handle;
}

bool SessionRegistry::StartLogin(void* owner, uint64_t handle) {
    std::shared_ptr<LoginSession> session;
    {
        IOLockGuard lock(lock_);
        auto* record = FindByHandleForOwner(owner, handle);
        if (!record || !record->session) {
            return false;
        }
        if (record->session->State() != LoginState::Idle) {
            return false;
        }

        std::weak_ptr<int> alive = lifetimeToken_;
        record->session->SetLoginCallback(
            [this, handle, alive](const LoginCompleteParams& params) {
                if (alive.expired()) {
                    return;  // registry destroyed; session outlived it
                }
                IOLockGuard cbLock(lock_);
                auto* rec = FindByHandle(handle);
                if (rec == nullptr) {
                    return;
                }
                rec->lastError = params.status;
            });
        session = record->session;
    }
    // Outside lock_: a synchronously failing management write invokes the login
    // callback INLINE, and the callback re-takes lock_ (os_unfair_lock recursion
    // aborts the dext).
    return session->Login();
}

std::optional<SBP2SessionState> SessionRegistry::GetSessionState(void* owner,
                                                                 uint64_t handle) const {
    IOLockGuard lock(lock_);
    const auto* record = FindByHandleForOwner(owner, handle);
    if (!record || !record->session) {
        return std::nullopt;
    }

    SBP2SessionState state{};
    state.loginState = record->session->State();
    state.loginID = record->session->LoginID();
    state.generation = record->session->Generation();
    state.lastError = record->lastError;
    state.reconnectPending = (state.loginState == LoginState::Suspended);
    return state;
}

bool SessionRegistry::SubmitInquiry(void* owner, uint64_t handle, uint8_t allocationLength) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandleForOwner(owner, handle);
    if (!record || !record->executor) {
        return false;
    }
    return record->executor->SubmitInquiry(allocationLength);
}

std::optional<SCSI::CommandResult> SessionRegistry::GetInquiryResult(void* owner, uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandleForOwner(owner, handle);
    if (!record || !record->executor) {
        return std::nullopt;
    }
    return record->executor->GetInquiryResult();
}

bool SessionRegistry::SubmitCommand(void* owner, uint64_t handle,
                                    const SCSI::CommandRequest& request,
                                    CommandExecutor::ResultCallback callback) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandleForOwner(owner, handle);
    if (!record || !record->executor) {
        return false;
    }
    return record->executor->SubmitCommand(request, std::move(callback));
}

std::optional<SCSI::CommandResult> SessionRegistry::GetCommandResult(void* owner, uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandleForOwner(owner, handle);
    if (!record || !record->executor) {
        return std::nullopt;
    }
    return record->executor->GetCommandResult();
}

bool SessionRegistry::SubmitTaskManagement(void* owner, uint64_t handle,
                                           SBP2ManagementORB::Function function) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandleForOwner(owner, handle);
    if (!record || !record->executor) {
        return false;
    }
    return record->executor->SubmitTaskManagement(function);
}

bool SessionRegistry::ReleaseSession(void* owner, uint64_t handle) {
    std::shared_ptr<LoginSession> toLogout;
    {
        IOLockGuard lock(lock_);
        auto it = sessions_.find(handle);
        if (it == sessions_.end() || it->second.owner != owner) {
            return false;
        }

        auto& record = it->second;
        if (record.executor) {
            record.executor->Cleanup();
        }

        const LoginState state = record.session ? record.session->State() : LoginState::Idle;
        if (record.session &&
            (state == LoginState::LoggedIn || state == LoginState::Suspended)) {
            SetReleaseLogoutCallbackLocked(handle, record.session);
            // Retire optimistically; un-retired below if Logout() is rejected.
            RetireSessionLocked(record);
            toLogout = record.session;
        } else if (record.session && state == LoginState::LoggingOut) {
            SetReleaseLogoutCallbackLocked(handle, record.session);
            RetireSessionLocked(record);
        }
        sessions_.erase(it);
    }

    // Outside lock_: a synchronously failing logout write invokes the logout
    // callback INLINE, and the callback re-takes lock_ (crashed the dext on HW
    // when the device vanished during teardown).
    if (toLogout && !toLogout->Logout()) {
        IOLockGuard lock(lock_);
        EraseRetiredSessionLocked(toLogout);
    }
    return true;
}

void SessionRegistry::ReleaseOwner(void* owner) {
    std::vector<std::shared_ptr<LoginSession>> toLogout;
    {
        IOLockGuard lock(lock_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second.owner != owner) {
                ++it;
                continue;
            }

            auto& record = it->second;
            const LoginState state = record.session ? record.session->State() : LoginState::Idle;

            if (record.session &&
                (state == LoginState::LoggedIn || state == LoginState::Suspended)) {
                SetReleaseLogoutCallbackLocked(record.handle, record.session);
                // Retire optimistically; un-retired below if Logout() is rejected.
                RetireSessionLocked(record);
                toLogout.push_back(record.session);
            } else if (record.session && state == LoginState::LoggingOut) {
                SetReleaseLogoutCallbackLocked(record.handle, record.session);
                RetireSessionLocked(record);
            }

            if (record.executor) {
                record.executor->Cleanup();
            }
            it = sessions_.erase(it);
        }
    }

    // Outside lock_ — see ReleaseSession.
    for (auto& session : toLogout) {
        if (!session->Logout()) {
            IOLockGuard lock(lock_);
            EraseRetiredSessionLocked(session);
        }
    }
}

void SessionRegistry::OnBusReset(uint16_t newGeneration) {
    IOLockGuard lock(lock_);
    for (auto& [handle, record] : sessions_) {
        if (record.session) {
            record.session->HandleBusReset(newGeneration);
        }
        if (record.executor) {
            record.executor->OnBusReset();
        }
    }
}

void SessionRegistry::RefreshTargets(Discovery::Generation gen) {
    std::vector<std::shared_ptr<LoginSession>> toReconnect;
    {
        IOLockGuard lock(lock_);
        for (auto& [handle, record] : sessions_) {
            if (!record.session || record.session->State() != LoginState::Suspended) {
                continue;
            }

            auto unit = ResolveUnit(record.guid, record.romOffset);
            if (!unit) {
                ASFW_LOG(Async, "SessionRegistry: RefreshTargets: unit not found for handle=%llu",
                         handle);
                continue;
            }

            record.session->Configure(BuildTargetInfoFromUnit(*unit));
            ASFW_LOG(Async, "SessionRegistry: reconnecting session handle=%llu gen=%u",
                     handle, gen.value);
            toReconnect.push_back(record.session);
        }
    }
    // Outside lock_ — a synchronously failing reconnect write completes inline
    // (same hazard class as Login/Logout, see ReleaseSession).
    for (auto& session : toReconnect) {
        (void)session->Reconnect();
    }
}

#ifdef ASFW_HOST_TEST
LoginSession* SessionRegistry::GetSessionForTesting(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    return record ? record->session.get() : nullptr;
}

std::weak_ptr<LoginSession> SessionRegistry::GetSessionWeakForTesting(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record) {
        return {};
    }
    return record->session;
}
#endif

SessionRecord* SessionRegistry::FindByHandle(uint64_t handle) {
    auto it = sessions_.find(handle);
    return it != sessions_.end() ? &it->second : nullptr;
}

const SessionRecord* SessionRegistry::FindByHandle(uint64_t handle) const {
    auto it = sessions_.find(handle);
    return it != sessions_.end() ? &it->second : nullptr;
}

SessionRecord* SessionRegistry::FindByHandleForOwner(void* owner, uint64_t handle) {
    auto* record = FindByHandle(handle);
    return (record != nullptr && record->owner == owner) ? record : nullptr;
}

const SessionRecord* SessionRegistry::FindByHandleForOwner(void* owner, uint64_t handle) const {
    const auto* record = FindByHandle(handle);
    return (record != nullptr && record->owner == owner) ? record : nullptr;
}

std::shared_ptr<Discovery::FWUnit> SessionRegistry::ResolveUnit(uint64_t guid,
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

bool SessionRegistry::HasSessionForTargetLocked(uint64_t guid, uint32_t romOffset) const {
    for (const auto& [handle, record] : sessions_) {
        if (record.guid == guid && record.romOffset == romOffset && record.session != nullptr) {
            return true;
        }
    }
    return std::any_of(retiringSessions_.begin(), retiringSessions_.end(),
                       [guid, romOffset](const RetiringSession& retired) {
                           return retired.guid == guid && retired.romOffset == romOffset &&
                                  retired.session != nullptr;
                       });
}

void SessionRegistry::RetireSessionLocked(const SessionRecord& record) {
    if (record.session == nullptr) {
        return;
    }
    const auto it = std::find_if(
        retiringSessions_.begin(), retiringSessions_.end(),
        [&record](const RetiringSession& retired) { return retired.session == record.session; });
    if (it == retiringSessions_.end()) {
        retiringSessions_.push_back(RetiringSession{
            .guid = record.guid,
            .romOffset = record.romOffset,
            .session = record.session,
        });
    }
}

void SessionRegistry::EraseRetiredSessionLocked(const std::shared_ptr<LoginSession>& session) {
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

void SessionRegistry::SetReleaseLogoutCallbackLocked(
    uint64_t handle, const std::shared_ptr<LoginSession>& session) {
    if (session == nullptr) {
        return;
    }
    // Callers must NOT hold lock_ when issuing the Logout() write: a
    // synchronously failing write invokes this callback inline, and it re-takes
    // lock_. The token guards against the session outliving the registry.
    std::weak_ptr<LoginSession> weakSession = session;
    std::weak_ptr<int> alive = lifetimeToken_;
    session->SetLogoutCallback([this, handle, weakSession, alive](const LogoutCompleteParams&) {
        if (alive.expired()) {
            return;  // registry destroyed; session outlived it
        }
        IOLockGuard cbLock(lock_);
        std::shared_ptr<LoginSession> completedSession = weakSession.lock();

        auto it = sessions_.find(handle);
        if (it != sessions_.end() &&
            (completedSession == nullptr || it->second.session == completedSession)) {
            if (it->second.executor) {
                it->second.executor->Cleanup();
            }
            sessions_.erase(it);
        }

        EraseRetiredSessionLocked(completedSession);
    });
}

} // namespace ASFW::Protocols::SBP2
