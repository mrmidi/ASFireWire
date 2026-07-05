#pragma once

// SessionRegistry — bridges discovery metadata to LoginSession instances.
//
// Decomposed from PR #19's SBP2SessionRegistry (§2a of SBP2_SESSION_PORT.md):
// identity & lifecycle only. Sessions are created for an owner and target
// (guid, romOffset); public operations use opaque handles plus owner validation,
// and the registry rejects duplicate live targets by (guid, romOffset). The
// command plane lives in a per-record CommandExecutor (§2b); command/inquiry/
// task-management calls validate here and delegate to it.
//
// DICE adaptations vs #19:
//   * LoginSession timers run on an injected ISessionScheduler (constructor
//     argument), not the two-queue model.
//   * ReleaseSession no longer blocks the queue with an IOSleep wait-loop; like
//     ReleaseOwner it starts logout, retires the session, and lets the async
//     logout completion (or its scheduler timeout) erase it.

#include "LoginSession.hpp"
#include "CommandExecutor.hpp"
#include "ISessionScheduler.hpp"
#include "../SBP2ManagementORB.hpp"
#include "../SCSICommandSet.hpp"
#include "../AddressSpaceManager.hpp"
#include "../../../Discovery/IDeviceManager.hpp"
#include "../../../Discovery/DiscoveryTypes.hpp"
#include "../../../Logging/Logging.hpp"

#include <algorithm>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ASFW::Protocols::SBP2 {

// SBP-2 Unit_Directory identity used by Nikon/modernscan and visible in raw ROM.
inline constexpr uint32_t kSBP2UnitSpecId = 0x00609E;
inline constexpr uint32_t kSBP2UnitSwVersion = 0x010483;

// Per-session state exposed to UserClient.
struct SBP2SessionState {
    LoginState loginState{LoginState::Idle};
    uint16_t loginID{0};
    uint16_t generation{0};
    int32_t lastError{0};
    bool reconnectPending{false};
};

// Slim per-session record (§2c). The command god-object that bloated #19's record
// now lives in `executor`.
struct SessionRecord {
    uint64_t handle{0};
    void* owner{nullptr};
    uint64_t guid{0};
    uint32_t romOffset{0};
    std::shared_ptr<LoginSession> session;
    int32_t lastError{0};
    // Declared last so it is destroyed first (it references `session`/`lastError`).
    std::unique_ptr<CommandExecutor> executor;
};

class SessionRegistry {
public:
    SessionRegistry(Async::IFireWireBus& bus,
                    Async::IFireWireBusInfo& busInfo,
                    AddressSpaceManager& addrSpaceMgr,
                    Discovery::IDeviceManager& deviceManager,
                    ISessionScheduler& scheduler,
                    IODispatchQueue* workQueue = nullptr);
    ~SessionRegistry();

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    [[nodiscard]] std::expected<uint64_t, int> CreateSession(void* owner,
                                                             uint64_t guid,
                                                             uint32_t romOffset);

    [[nodiscard]] bool StartLogin(void* owner, uint64_t handle);

    [[nodiscard]] std::optional<SBP2SessionState> GetSessionState(void* owner,
                                                                  uint64_t handle) const;

    [[nodiscard]] bool SubmitInquiry(void* owner, uint64_t handle, uint8_t allocationLength = 96);
    [[nodiscard]] std::optional<SCSI::CommandResult> GetInquiryResult(void* owner, uint64_t handle);

    [[nodiscard]] bool SubmitCommand(void* owner, uint64_t handle,
                                     const SCSI::CommandRequest& request,
                                     CommandExecutor::ResultCallback callback = {});
    [[nodiscard]] std::optional<SCSI::CommandResult> GetCommandResult(void* owner, uint64_t handle);

    [[nodiscard]] bool SubmitTaskManagement(void* owner, uint64_t handle,
                                            SBP2ManagementORB::Function function);

    [[nodiscard]] bool ReleaseSession(void* owner, uint64_t handle);
    void ReleaseOwner(void* owner);

    void OnBusReset(uint16_t newGeneration);
    void RefreshTargets(Discovery::Generation gen);

    // Wire the last-resort bus-reset hook handed to every CommandExecutor (see
    // CommandExecutor::SetBusResetRequester). Set once during driver wiring,
    // before any session exists.
    void SetBusResetRequester(std::function<void()> requester);

#ifdef ASFW_HOST_TEST
    LoginSession* GetSessionForTesting(uint64_t handle);
    std::weak_ptr<LoginSession> GetSessionWeakForTesting(uint64_t handle);
#endif

private:
    SessionRecord* FindByHandle(uint64_t handle);
    const SessionRecord* FindByHandle(uint64_t handle) const;
    SessionRecord* FindByHandleForOwner(void* owner, uint64_t handle);
    const SessionRecord* FindByHandleForOwner(void* owner, uint64_t handle) const;

    std::shared_ptr<Discovery::FWUnit> ResolveUnit(uint64_t guid, uint32_t romOffset) const;

    [[nodiscard]] bool HasSessionForTargetLocked(uint64_t guid, uint32_t romOffset) const;
    void RetireSessionLocked(const SessionRecord& record);
    void EraseRetiredSessionLocked(const std::shared_ptr<LoginSession>& session);
    void SetReleaseLogoutCallbackLocked(uint64_t handle,
                                        const std::shared_ptr<LoginSession>& session);

    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    AddressSpaceManager& addrSpaceMgr_;
    Discovery::IDeviceManager& deviceManager_;
    ISessionScheduler& scheduler_;
    IODispatchQueue* workQueue_{nullptr};

    IOLock* lock_{nullptr};
    std::function<void()> busResetRequester_;
    std::map<uint64_t, SessionRecord> sessions_;
    struct RetiringSession {
        uint64_t guid{0};
        uint32_t romOffset{0};
        std::shared_ptr<LoginSession> session;
    };
    // Hidden from registry clients, but retained until async logout finishes/times out.
    std::vector<RetiringSession> retiringSessions_;
    uint64_t nextHandle_{1};

    // Guards deferred login/logout completion bodies (they re-take lock_, so
    // they are dispatched to workQueue_ — a management write can fail INLINE
    // while the caller still holds lock_, and os_unfair_lock recursion aborts).
    std::shared_ptr<int> lifetimeToken_{std::make_shared<int>(0)};
};

} // namespace ASFW::Protocols::SBP2
