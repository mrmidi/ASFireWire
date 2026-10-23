#pragma once

// LoginSession — SBP-2 login/reconnect/logout state machine (management plane).
//
// Decomposed from PR #19's SBP2LoginSession (1847 lines). The post-login command
// plane (fetch-agent ORB submission, status→ORB matching, agent reset) lives in
// the composed FetchAgent; this class owns only the
// management plane:
//
//   1. Configure() with ROM-derived target parameters.
//   2. Login() — write the Login ORB address to the device's management agent.
//   3. On the status block write, parse the login response, derive the
//      command-block-agent addresses, Bind() the FetchAgent, and write the
//      busy-timeout CSR.
//   4. On bus reset, suspend; Resume() after rediscovery reconnects a held
//      login or logs in again if the login never completed.
//   5. Logout() terminates the session.
//
// The states and their transitions are LoginFsm (LoginStateMachine.hpp); this
// class feeds it events and carries out the effects.
//
// DICE adaptations vs #19:
//   * Timers go through the injected ISessionScheduler (one cancelable management
//     timer at a time), never the IOSleep-on-queue path. The two-queue model
//     (work + owned timeout queue) is removed (§4).
//   * Device-visible memory is allocated through AddressSpaceManager (handles, not
//     raw buffers). The status-FIFO remote-write callback captures weak_from_this.
//   * Command submission delegates to the FetchAgent; status routing forwards
//     solicited blocks to FetchAgent::OnStatusBlock.

#include "ISessionScheduler.hpp"
#include "LoginStateMachine.hpp"
#include "FetchAgent.hpp"
#include "../SBP2WireFormats.hpp"
#include "../SBP2CommandORB.hpp"
#include "../AddressSpaceManager.hpp"
#include "../../../Async/AsyncTypes.hpp"
#include "../../../Logging/Logging.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::Async {
class IFireWireBus;
class IFireWireBusInfo;
}

namespace ASFW::Protocols::SBP2 {

// ---------------------------------------------------------------------------
// Configuration parameters (from Config ROM Unit_Directory parsing)
// ---------------------------------------------------------------------------

struct SBP2TargetInfo {
    uint32_t managementAgentOffset{0};   // From Management_Agent_Offset key
    uint16_t lun{0};                     // Logical unit number

    // From Unit_Characteristics key (if present)
    uint32_t managementTimeoutMs{2000};  // Unit_Characteristics[15:8] * 500 ms
    uint16_t maxORBSize{32};             // Unit_Characteristics[7:0] * 4, min 32
    uint16_t maxCommandBlockSize{0};     // maxORBSize - sizeof(NormalORB header)

    // From Fast_Start key (optional)
    bool fastStartSupported{false};
    uint8_t fastStartOffset{0};
    uint8_t fastStartMaxPayload{0};

    // Target node (from discovery)
    uint16_t targetNodeId{0xFFFF};
};

// ---------------------------------------------------------------------------
// Completion callback parameters
// ---------------------------------------------------------------------------

struct LoginCompleteParams {
    int status{0};  // 0 = success, negative = errno-style error
    Wire::LoginResponse loginResponse{};
    uint16_t generation{0};
};

struct LogoutCompleteParams {
    int status{0};
    uint16_t generation{0};
};

// What other queues read: one atomic word, so the state, login ID and
// generation are always from the same transition.
struct LoginSnapshot {
    LoginState state{LoginState::Idle};
    uint16_t generation{0};
    std::optional<uint16_t> loginID;  // set only while the target holds our login
};

// ---------------------------------------------------------------------------
// LoginSession
// ---------------------------------------------------------------------------

class LoginSession : public std::enable_shared_from_this<LoginSession> {
    friend class SessionRegistry;

public:
    using LoginCallback = std::function<void(const LoginCompleteParams&)>;
    using LogoutCallback = std::function<void(const LogoutCompleteParams&)>;
    using StatusCallback = std::function<void(const Wire::StatusBlock&, uint32_t length)>;
    // Admits the target's node to OHCI physical DMA for one generation.
    using PhysicalDmaEnabler = std::function<std::expected<void, Driver::PhysicalDmaError>(
        uint16_t node, uint16_t generation)>;

    LoginSession(Async::IFireWireBus& bus,
                 Async::IFireWireBusInfo& busInfo,
                 AddressSpaceManager& addrSpaceMgr,
                 ISessionScheduler& scheduler);
    ~LoginSession();

    LoginSession(const LoginSession&) = delete;
    LoginSession& operator=(const LoginSession&) = delete;

    // -----------------------------------------------------------------------
    // Configuration (call once before Login)
    // -----------------------------------------------------------------------

    void Configure(const SBP2TargetInfo& info) noexcept;
    void SetLoginCallback(LoginCallback cb) noexcept { loginCallback_ = std::move(cb); }
    void SetLogoutCallback(LogoutCallback cb) noexcept { logoutCallback_ = std::move(cb); }
    void SetStatusCallback(StatusCallback cb) noexcept { statusCallback_ = std::move(cb); }
    void SetPhysicalDmaEnabler(PhysicalDmaEnabler fn) noexcept { physicalDma_ = std::move(fn); }

    // -----------------------------------------------------------------------
    // Session operations
    // -----------------------------------------------------------------------

    [[nodiscard]] bool Login() noexcept;
    [[nodiscard]] bool Logout() noexcept;
    // Rediscovery supplied a fresh route (Configure() first): reconnect a held
    // login, or log in if the reset interrupted the login.
    [[nodiscard]] bool Resume() noexcept;
    void HandleBusReset(uint16_t newGeneration) noexcept;
    // The suspended login's device is gone for good (unit terminated, or back
    // as a new incarnation that cannot hold the old login). No reconnect is
    // possible: go Failed and report the session lost. No bus traffic.
    void AbandonSuspended() noexcept;

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    // Safe from any queue.
    [[nodiscard]] LoginSnapshot Snapshot() const noexcept;
    [[nodiscard]] LoginState State() const noexcept { return Snapshot().state; }

    [[nodiscard]] const SBP2TargetInfo& TargetInfo() const noexcept { return targetInfo_; }
    // Read the state unserialized: host tests and diagnostics only.
    [[nodiscard]] Async::FWAddress CommandBlockAgent() const noexcept;
    [[nodiscard]] uint32_t ReconnectHoldSeconds() const noexcept;
    [[nodiscard]] uint16_t MaxPayloadSize() const noexcept { return maxPayloadSize_; }
    void SetMaxPayloadSize(uint16_t bytes) noexcept { maxPayloadSize_ = bytes; }

    // -----------------------------------------------------------------------
    // Command plane (delegates to FetchAgent)
    // -----------------------------------------------------------------------

    /// Submit a Normal Command ORB. Requires LoggedIn. Returns false if not
    /// logged in or the FetchAgent rejects the ORB.
    [[nodiscard]] bool SubmitORB(SBP2CommandORB* orb) noexcept;

    /// Reset the fetch agent. Completion via callback.
    void ResetFetchAgent(std::function<void(int)> callback) noexcept;

    /// AGENT_RESET without tearing down ORB tracking; onComplete runs when the
    /// write completes, whatever its status.
    void ResetFetchAgentNoWait(std::function<void()> onComplete) noexcept {
        fetchAgent_.ResetNoWait(std::move(onComplete));
    }

    /// Drop all outstanding command-ORB tracking and cancel any in-flight
    /// fetch-agent write. Used by the command plane (CommandExecutor)
    /// when a command completes, fails, or is aborted — mirrors #19's
    /// ClearORBTracking, which CleanupCommandResources invoked on the session.
    void ClearCommandTracking() noexcept { fetchAgent_.Clear(true); }

    /// Re-enable unsolicited status after the device sends one. If called before
    /// login completes, it is deferred until LoggedIn.
    void EnableUnsolicitedStatus() noexcept;

    // Test seam: override the FetchAgent's fetch-agent-write retry budget.
    void SetFetchAgentWriteRetriesForTesting(uint32_t retries) noexcept {
        fetchAgent_.SetWriteRetriesForTesting(retries);
    }

private:
    // Resource allocation -----------------------------------------------------
    bool AllocateResources() noexcept;
    void DeallocateResources() noexcept;
    bool AllocateLoginORBAddressSpace() noexcept;
    bool AllocateLoginResponseAddressSpace() noexcept;
    bool AllocateStatusBlockAddressSpace() noexcept;
    bool AllocateReconnectORBAddressSpace() noexcept;
    bool AllocateLogoutORBAddressSpace() noexcept;
    void RegisterStatusBlockCallback() noexcept;

    // State machine ----------------------------------------------------------
    // Every access to fsm_ runs through Serialized(), one piece of work at a
    // time whatever the calling queue: timers, completions and RX arrive on the
    // driver queue, the probe user client calls in from its own. The lock
    // covers only the queue, so work may run callbacks that take the registry's
    // lock. Work raised while other work runs (an inline write completion, a
    // callback calling back in) is queued behind it: run-to-completion.
    void Serialized(std::function<void()> work) noexcept;
    void Dispatch(LoginFsm::Event event) noexcept;
    // A public request: false if the state refused it, or its write was refused
    // on the spot. Queued behind another thread's work, it reports true.
    [[nodiscard]] bool Request(LoginFsm::Event event, const char* what) noexcept;
    bool Apply(const LoginFsm::Event& event) noexcept;
    void Commit(LoginFsm::State next) noexcept;
    [[nodiscard]] LoginFsm::Route Now() const noexcept;
    void IssueWrite(LoginFsm::Op op) noexcept;
    void ArmTimer(LoginFsm::Timer timer) noexcept;
    void Notify(const LoginFsm::Effects& fx) noexcept;

    // ORB construction --------------------------------------------------------
    void BuildLoginORB() noexcept;
    void BuildReconnectORB(uint16_t loginID) noexcept;
    void BuildLogoutORB(uint16_t loginID) noexcept;
    [[nodiscard]] const std::array<uint8_t, 8>& OrbAddressFor(LoginFsm::Op op) const noexcept;

    // Bind the command plane after a login or reconnect -----------------------
    void BindCommandPlane(LoginFsm::Op op) noexcept;
    void BindFetchAgent(const LoginFsm::HeldLogin& login, const LoginFsm::Route& route) noexcept;

    void NotifySessionLost(uint16_t generation) noexcept;

    // Status block handling ---------------------------------------------------
    void OnStatusBlockRemoteWrite(uint32_t offset, std::span<const uint8_t> payload) noexcept;
    void ProcessStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;
    void CompleteLoginFromStatusBlock(const Wire::StatusBlock& block) noexcept;
    [[nodiscard]] bool EnablePhysicalDma(const LoginFsm::Route& route) noexcept;
    void CompleteReconnectFromStatusBlock(const Wire::StatusBlock& block) noexcept;

    // Unsolicited status + busy timeout writes -------------------------------
    void EnableUnsolicitedStatusNow() noexcept;
    void OnUnsolicitedStatusEnableComplete(uint16_t expectedGeneration,
                                           Async::AsyncStatus status) noexcept;
    void WriteBusyTimeout() noexcept;
    void OnBusyTimeoutComplete(uint16_t expectedGeneration, Async::AsyncStatus status) noexcept;

    // Helpers -----------------------------------------------------------------
    void ArmManagementTimer(uint64_t delayMs, LoginFsm::Event event) noexcept;
    void CancelManagementTimer() noexcept;

    [[nodiscard]] static FW::NodeId ShortNode(uint16_t node) noexcept {
        return FW::NodeId{static_cast<uint8_t>(node & 0x3Fu)};
    }

    // Members -----------------------------------------------------------------
    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    AddressSpaceManager& addrSpaceMgr_;
    ISessionScheduler& scheduler_;
    FetchAgent fetchAgent_;

    // Configuration
    SBP2TargetInfo targetInfo_{};
    PhysicalDmaEnabler physicalDma_;
    bool configured_{false};
    uint16_t maxPayloadSize_{4096};  // default, clipped by login response

    // Session state. fsm_ lives on the driver work queue; other queues (the
    // HBA via SessionRegistry::GetSessionState, the user client) read snapshot_.
    LoginFsm::State fsm_{LoginFsm::St::Idle{}};
    std::atomic<uint64_t> snapshot_{0};
    IOLock* executorLock_{nullptr};
    std::deque<std::function<void()>> pendingWork_;
    bool executing_{false};

    static constexpr uint64_t kRetryDelayMs = 1000;

    // Address space handles ---------------------------------------------------
    uint64_t loginORBHandle_{0};
    AddressSpaceManager::AddressRangeMeta loginORBMeta_{};
    Wire::LoginORB loginORBBuffer_{};

    uint64_t loginResponseHandle_{0};
    AddressSpaceManager::AddressRangeMeta loginResponseMeta_{};

    uint64_t statusBlockHandle_{0};
    AddressSpaceManager::AddressRangeMeta statusBlockMeta_{};

    uint64_t reconnectORBHandle_{0};
    AddressSpaceManager::AddressRangeMeta reconnectORBMeta_{};
    Wire::ReconnectORB reconnectORBBuffer_{};

    uint64_t logoutORBHandle_{0};
    AddressSpaceManager::AddressRangeMeta logoutORBMeta_{};
    Wire::LogoutORB logoutORBBuffer_{};

    // 8-byte big-endian ORB addresses for the management agent write
    std::array<uint8_t, 8> loginORBAddressBE_{};
    std::array<uint8_t, 8> reconnectORBAddressBE_{};
    std::array<uint8_t, 8> logoutORBAddressBE_{};

    // In-flight management writes
    Async::AsyncHandle loginWriteHandle_{};
    Async::AsyncHandle reconnectWriteHandle_{};
    Async::AsyncHandle logoutWriteHandle_{};

    // Unsolicited status enable
    Async::FWAddress unsolicitedStatusAddress_{};
    Async::AsyncHandle unsolicitedStatusWriteHandle_{};
    bool unsolicitedStatusRequested_{false};

    // Busy timeout CSR write
    static constexpr uint32_t kCSRBusAddressHi = 0x0000FFFFu;
    static constexpr uint32_t kBusyTimeoutAddressLo = 0xF0000210u;
    static constexpr uint32_t kBusyTimeoutValue = 0x0000000Fu;
    Async::AsyncHandle busyTimeoutWriteHandle_{};
    bool busyTimeoutInProgress_{false};
    uint32_t busyTimeoutBuffer_{OSSwapHostToBigInt32(kBusyTimeoutValue)};

    // Callbacks
    LoginCallback loginCallback_;
    LogoutCallback logoutCallback_;
    StatusCallback statusCallback_;

    // Single cancelable management-plane timer (login/reconnect/logout/retry).
    SchedulerToken managementTimerToken_{kInvalidSchedulerToken};
    uint64_t timerArm_{0};
};

} // namespace ASFW::Protocols::SBP2
