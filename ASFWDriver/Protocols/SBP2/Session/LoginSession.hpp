#pragma once

// LoginSession — SBP-2 login/reconnect/logout state machine (management plane).
//
// Decomposed from PR #19's SBP2LoginSession (1847 lines). The post-login command
// plane (fetch-agent ORB submission, doorbell, status→ORB matching, agent reset)
// lives in the composed FetchAgent (§1b of SBP2_SESSION_PORT.md); this class owns
// only the management plane:
//
//   1. Configure() with ROM-derived target parameters.
//   2. Login() — write the Login ORB address to the device's management agent.
//   3. On the status block write, parse the login response, derive the
//      command-block-agent addresses, Bind() the FetchAgent, and write the
//      busy-timeout CSR.
//   4. On bus reset, Suspend then Reconnect() with the stored loginID.
//   5. Logout() terminates the session.
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
#include "FetchAgent.hpp"
#include "../SBP2WireFormats.hpp"
#include "../SBP2CommandORB.hpp"
#include "../AddressSpaceManager.hpp"
#include "../../../Async/AsyncTypes.hpp"
#include "../../../Logging/Logging.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

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
    Wire::StatusBlock statusBlock{};
    uint32_t statusBlockLength{0};
    uint16_t generation{0};
};

struct LogoutCompleteParams {
    int status{0};
    uint16_t generation{0};
};

// ---------------------------------------------------------------------------
// Login session states
// ---------------------------------------------------------------------------

enum class LoginState : uint8_t {
    Idle,
    LoggingIn,
    LoggedIn,
    Reconnecting,
    LoggingOut,
    Suspended,     // Lost after bus reset, waiting for reconnect
    Failed
};

[[nodiscard]] inline constexpr const char* ToString(LoginState s) noexcept {
    switch (s) {
        case LoginState::Idle:         return "Idle";
        case LoginState::LoggingIn:    return "LoggingIn";
        case LoginState::LoggedIn:     return "LoggedIn";
        case LoginState::Reconnecting: return "Reconnecting";
        case LoginState::LoggingOut:   return "LoggingOut";
        case LoginState::Suspended:    return "Suspended";
        case LoginState::Failed:       return "Failed";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// LoginSession
// ---------------------------------------------------------------------------

class LoginSession : public std::enable_shared_from_this<LoginSession> {
    friend class SessionRegistry;

public:
    using LoginCallback = std::function<void(const LoginCompleteParams&)>;
    using LogoutCallback = std::function<void(const LogoutCompleteParams&)>;
    using StatusCallback = std::function<void(const Wire::StatusBlock&, uint32_t length)>;

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

    // -----------------------------------------------------------------------
    // Session operations
    // -----------------------------------------------------------------------

    [[nodiscard]] bool Login() noexcept;
    [[nodiscard]] bool Logout() noexcept;
    [[nodiscard]] bool Reconnect() noexcept;
    void HandleBusReset(uint16_t newGeneration) noexcept;

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] LoginState State() const noexcept { return state_; }
    [[nodiscard]] uint16_t LoginID() const noexcept { return loginID_; }
    [[nodiscard]] uint16_t Generation() const noexcept { return loginGeneration_; }
    [[nodiscard]] const SBP2TargetInfo& TargetInfo() const noexcept { return targetInfo_; }
    [[nodiscard]] Async::FWAddress CommandBlockAgent() const noexcept { return commandBlockAgent_; }
    [[nodiscard]] uint32_t ReconnectHoldSeconds() const noexcept;
    [[nodiscard]] uint16_t MaxPayloadSize() const noexcept { return maxPayloadSize_; }
    void SetMaxPayloadSize(uint16_t bytes) noexcept { maxPayloadSize_ = bytes; }

    // -----------------------------------------------------------------------
    // Command plane (delegates to FetchAgent)
    // -----------------------------------------------------------------------

    /// Submit a Normal Command ORB. Requires LoggedIn. Returns false if not
    /// logged in or the FetchAgent rejects the ORB.
    [[nodiscard]] bool SubmitORB(SBP2CommandORB* orb) noexcept;

    /// Reset the fetch agent (clears the ORB chain). Completion via callback.
    void ResetFetchAgent(std::function<void(int)> callback) noexcept;

    /// Drop all outstanding command-ORB tracking and cancel any in-flight
    /// fetch-agent / doorbell write. Used by the command plane (CommandExecutor)
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

    // ORB construction --------------------------------------------------------
    void BuildLoginORB() noexcept;
    void BuildReconnectORB() noexcept;
    void BuildLogoutORB() noexcept;

    // Bind / unbind the command plane ----------------------------------------
    void BindFetchAgent() noexcept;
    void RefreshUnsolicitedStatusAddress() noexcept;

    // Management write completions -------------------------------------------
    void OnLoginWriteComplete(uint16_t expectedGeneration, Async::AsyncStatus status) noexcept;
    void OnLoginTimeout() noexcept;
    void OnReconnectWriteComplete(uint16_t expectedGeneration, Async::AsyncStatus status) noexcept;
    void OnReconnectTimeout() noexcept;
    void OnLogoutWriteComplete(uint16_t expectedGeneration, Async::AsyncStatus status) noexcept;
    void OnLogoutTimeout() noexcept;

    // Status block handling ---------------------------------------------------
    void OnStatusBlockRemoteWrite(uint32_t offset, std::span<const uint8_t> payload) noexcept;
    void ProcessStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;
    void CompleteLoginFromStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;
    void CompleteReconnectFromStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;
    void CompleteLogoutFromStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;

    // Unsolicited status + busy timeout writes -------------------------------
    void OnUnsolicitedStatusEnableComplete(uint16_t expectedGeneration,
                                           Async::AsyncStatus status) noexcept;
    void WriteBusyTimeout() noexcept;
    void OnBusyTimeoutComplete(uint16_t expectedGeneration, Async::AsyncStatus status) noexcept;

    // Helpers -----------------------------------------------------------------
    void SetState(LoginState newState) noexcept;
    void StartLoginTimer() noexcept;
    void StartReconnectTimer() noexcept;
    void StartLogoutTimer() noexcept;
    void CancelLoginTimer() noexcept { CancelManagementTimer(); }
    void ArmManagementTimer(uint64_t delayMs, std::function<void()> fn) noexcept;
    void CancelManagementTimer() noexcept;
    void ScheduleManagementRetry(uint64_t delayMs, void (LoginSession::*op)()) noexcept;

    [[nodiscard]] uint16_t TargetNodeShort() const noexcept {
        return static_cast<uint16_t>(loginNodeID_ & 0x3Fu);
    }

    // Members -----------------------------------------------------------------
    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    AddressSpaceManager& addrSpaceMgr_;
    ISessionScheduler& scheduler_;
    FetchAgent fetchAgent_;

    // Configuration
    SBP2TargetInfo targetInfo_{};
    bool configured_{false};
    uint16_t maxPayloadSize_{4096};  // default, clipped by login response

    // Session state
    LoginState state_{LoginState::Idle};
    uint16_t loginID_{0};
    uint16_t loginGeneration_{0};
    uint16_t loginNodeID_{0xFFFF};

    // Login response data
    Wire::LoginResponse loginResponse_{};
    Async::FWAddress commandBlockAgent_{};
    uint16_t reconnectHold_{0};

    // Login retry state
    uint32_t loginRetryCount_{0};
    static constexpr uint32_t kLoginRetryMax = 32;
    static constexpr uint64_t kLoginRetryDelayMs = 1000;

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
};

} // namespace ASFW::Protocols::SBP2
