#pragma once

// SBP-2 Login/Reconnect/Logout state machine for ASFW.
// Ported from Apple IOFireWireSBP2Login.cpp — simplified for DriverKit.
//
// Lifecycle:
//   1. Create SBP2LoginSession with bus + address-space deps
//   2. Call Configure() with ROM-derived parameters (management offset, LUN, etc.)
//   3. Call Login() — sends login ORB to device's management agent
//   4. On success, session is kLoggedIn — can submit ORBs via fetch agent
//   5. On bus reset, auto Reconnect() with stored loginID
//   6. Call Logout() to terminate session

#include "SBP2WireFormats.hpp"
#include "SBP2CommandORB.hpp"
#include "SBP2ManagementORB.hpp"
#include "AddressSpaceManager.hpp"
#include "../../Async/AsyncTypes.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#endif

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>

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
    uint32_t managementTimeoutMs{2000};  // (byte[1] of unitCharacteristics) * 500 ms
    uint16_t maxORBSize{32};             // (byte[0] * 4), min 32
    uint16_t maxCommandBlockSize{0};     // maxORBSize - sizeof(NormalORB header)

    // From Fast_Start key (optional)
    bool fastStartSupported{false};
    uint8_t fastStartOffset{0};
    uint8_t fastStartMaxPayload{0};

    // Target node (from discovery)
    uint16_t targetNodeId{0xFFFF};
};

// ---------------------------------------------------------------------------
// Login completion callback parameters
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
// SBP2LoginSession
// ---------------------------------------------------------------------------

class SBP2LoginSession {
public:
    using LoginCallback = std::function<void(const LoginCompleteParams&)>;
    using LogoutCallback = std::function<void(const LogoutCompleteParams&)>;
    using StatusCallback = std::function<void(const Wire::StatusBlock&, uint32_t length)>;

    SBP2LoginSession(Async::IFireWireBus& bus,
                     Async::IFireWireBusInfo& busInfo,
                     AddressSpaceManager& addrSpaceMgr);
    ~SBP2LoginSession();

    SBP2LoginSession(const SBP2LoginSession&) = delete;
    SBP2LoginSession& operator=(const SBP2LoginSession&) = delete;

    // -----------------------------------------------------------------------
    // Configuration (call once before Login)
    // -----------------------------------------------------------------------

    /// Configure target parameters from Config ROM. Must be called before Login().
    void Configure(const SBP2TargetInfo& info) noexcept;

    /// Set login completion callback.
    void SetLoginCallback(LoginCallback cb) noexcept { loginCallback_ = std::move(cb); }

    /// Set logout completion callback.
    void SetLogoutCallback(LogoutCallback cb) noexcept { logoutCallback_ = std::move(cb); }

    /// Set status block notification callback (receives solicited + unsolicited status).
    void SetStatusCallback(StatusCallback cb) noexcept { statusCallback_ = std::move(cb); }

    /// Bind the IODispatchQueue used for delayed callbacks (timers).
    /// Must be called before Login() for timeout/retry support.
    void SetWorkQueue(IODispatchQueue* queue) noexcept;
    void SetTimeoutQueue(IODispatchQueue* queue) noexcept;

    // -----------------------------------------------------------------------
    // Session operations
    // -----------------------------------------------------------------------

    /// Initiate login to device. Completion via loginCallback_.
    /// Returns false if already logged in or configuration missing.
    [[nodiscard]] bool Login() noexcept;

    /// Initiate logout. Completion via logoutCallback_.
    [[nodiscard]] bool Logout() noexcept;

    /// Reconnect after bus reset. Called automatically or manually.
    [[nodiscard]] bool Reconnect() noexcept;

    /// Handle bus reset notification — transitions to Suspended if logged in.
    void HandleBusReset(uint16_t newGeneration) noexcept;

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    [[nodiscard]] LoginState State() const noexcept { return state_; }
    [[nodiscard]] uint16_t LoginID() const noexcept { return loginID_; }
    [[nodiscard]] uint16_t Generation() const noexcept { return loginGeneration_; }
    [[nodiscard]] const SBP2TargetInfo& TargetInfo() const noexcept { return targetInfo_; }

    /// Command Block Agent address (valid after successful login).
    [[nodiscard]] Async::FWAddress CommandBlockAgent() const noexcept;

    /// Get negotiated reconnect hold time (seconds).
    [[nodiscard]] uint32_t ReconnectHoldSeconds() const noexcept;

    /// Get max payload size for ORBs (bytes).
    [[nodiscard]] uint16_t MaxPayloadSize() const noexcept { return maxPayloadSize_; }

    /// Set max payload size override (clipped by login response).
    void SetMaxPayloadSize(uint16_t bytes) noexcept { maxPayloadSize_ = bytes; }

    // -----------------------------------------------------------------------
    // ORB submission
    // -----------------------------------------------------------------------

    /// Submit a Normal Command ORB to the device's fetch agent.
    /// Requires LoggedIn state. ORB must be fully configured before calling.
    [[nodiscard]] bool SubmitORB(SBP2CommandORB* orb) noexcept;

    /// Submit a management ORB (abort task, reset, etc).
    /// Requires LoggedIn state. ORB must be fully configured before calling.
    [[nodiscard]] bool SubmitManagementORB(SBP2ManagementORB* orb) noexcept;

    /// Reset the fetch agent. Clears ORB chain. Completion via callback.
    void ResetFetchAgent(std::function<void(int)> callback) noexcept;

    /// Re-enable unsolicited status after device sends one.
    void EnableUnsolicitedStatus() noexcept;

private:
    // -----------------------------------------------------------------------
    // Internal: resource allocation
    // -----------------------------------------------------------------------

    bool AllocateResources() noexcept;
    void DeallocateResources() noexcept;

    bool AllocateLoginORBAddressSpace() noexcept;
    bool AllocateLoginResponseAddressSpace() noexcept;
    bool AllocateStatusBlockAddressSpace() noexcept;
    bool AllocateReconnectORBAddressSpace() noexcept;
    bool AllocateLogoutORBAddressSpace() noexcept;

    // -----------------------------------------------------------------------
    // Internal: ORB construction and submission
    // -----------------------------------------------------------------------

    void BuildLoginORB() noexcept;
    void BuildReconnectORB() noexcept;
    void BuildLogoutORB() noexcept;

    // -----------------------------------------------------------------------
    // Internal: completion handlers
    // -----------------------------------------------------------------------

    void OnLoginWriteComplete(uint16_t expectedGeneration,
                              Async::AsyncStatus status,
                              std::span<const uint8_t> response) noexcept;
    void OnLoginTimeout() noexcept;
    void OnReconnectWriteComplete(uint16_t expectedGeneration,
                                  Async::AsyncStatus status,
                                  std::span<const uint8_t> response) noexcept;
    void OnReconnectTimeout() noexcept;
    void OnLogoutWriteComplete(uint16_t expectedGeneration,
                               Async::AsyncStatus status,
                               std::span<const uint8_t> response) noexcept;
    void OnLogoutTimeout() noexcept;

    // -----------------------------------------------------------------------
    // Internal: status block handling
    // -----------------------------------------------------------------------

    /// Called by AddressSpaceManager remote-write callback when the device
    /// writes a status block. Dispatches to the appropriate state handler.
    void OnStatusBlockRemoteWrite(uint32_t offset, std::span<const uint8_t> payload) noexcept;

    /// Parse and dispatch a received status block.
    void ProcessStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;

    // Internal: login/reconnect completion via status block
    void CompleteLoginFromStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;
    void CompleteReconnectFromStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;
    void CompleteLogoutFromStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept;

    // -----------------------------------------------------------------------
    // Internal: helpers
    // -----------------------------------------------------------------------

    void SetState(LoginState newState) noexcept;
    void StartLoginTimer() noexcept;
    void CancelLoginTimer() noexcept;

    /// Submit a delayed callback via IOTimerDispatchSource.
    void SubmitDelayedCallback(uint64_t delayMs,
                               std::function<void()> callback) noexcept;

    /// Cancel any pending timer callback.
    void CancelPendingTimer() noexcept;
    void EnsureTimeoutQueue() noexcept;
    void ReleaseOwnedTimeoutQueue() noexcept;
    [[nodiscard]] IODispatchQueue* EffectiveTimeoutQueue() const noexcept;
    void ClearORBTracking(bool cancelTimers) noexcept;
    [[nodiscard]] static uint64_t MakeORBKey(uint16_t addressHi, uint32_t addressLo) noexcept;
    [[nodiscard]] static uint64_t MakeORBKey(const Async::FWAddress& address) noexcept;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    AddressSpaceManager& addrSpaceMgr_;

    // Configuration
    SBP2TargetInfo targetInfo_{};
    bool configured_{false};
    uint16_t maxPayloadSize_{4096}; // default, clipped by login

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

    // -----------------------------------------------------------------------
    // Address space handles (from AddressSpaceManager)
    // -----------------------------------------------------------------------

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

    // ORB addresses as big-endian for management agent write (8 bytes each)
    std::array<uint8_t, 8> loginORBAddressBE_{};
    std::array<uint8_t, 8> reconnectORBAddressBE_{};
    std::array<uint8_t, 8> logoutORBAddressBE_{};

    // -----------------------------------------------------------------------
    // Async handles for in-flight operations
    // -----------------------------------------------------------------------

    Async::AsyncHandle loginWriteHandle_{};
    Async::AsyncHandle reconnectWriteHandle_{};
    Async::AsyncHandle logoutWriteHandle_{};

    bool loginTimerActive_{false};
    bool reconnectTimerActive_{false};
    bool logoutTimerActive_{false};

    // -----------------------------------------------------------------------
    // Callbacks
    // -----------------------------------------------------------------------

    LoginCallback loginCallback_;
    LogoutCallback logoutCallback_;
    StatusCallback statusCallback_;

    // -----------------------------------------------------------------------
    // Timer infrastructure
    // -----------------------------------------------------------------------

    IODispatchQueue* workQueue_{nullptr};
    IODispatchQueue* timeoutQueue_{nullptr};
#ifdef ASFW_HOST_TEST
    std::unique_ptr<IODispatchQueue> ownedTimeoutQueue_{};
#else
    IODispatchQueue* ownedTimeoutQueue_{nullptr};
#endif
    std::atomic<uint64_t> delayedCallbackGeneration_{0};
    std::shared_ptr<int> lifetimeToken_{std::make_shared<int>(0)};

    // -----------------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------------

    static constexpr uint16_t kCSRBusAddressHi = 0x0000FFFFu;
    static constexpr uint32_t kBusyTimeoutAddressLo = 0xF0000210u;
    static constexpr uint32_t kBusyTimeoutValue = 0x0000000Fu;

    // -----------------------------------------------------------------------
    // Fetch Agent / Doorbell internals
    // -----------------------------------------------------------------------

    /// Write ORB address to fetch agent (CBA + kORBPointer).
    bool AppendORBImmediate(SBP2CommandORB* orb) noexcept;
    void FailSubmittedORB(SBP2CommandORB* orb,
                          int transportStatus,
                          uint8_t sbpStatus) noexcept;

    /// Chain ORB to last ORB's next pointer.
    [[nodiscard]] bool AppendORB(SBP2CommandORB* orb) noexcept;

    /// Ring doorbell (write quadlet to CBA + kDoorbell).
    void RingDoorbell() noexcept;

    /// Fetch agent write completion handler.
    void OnFetchAgentWriteComplete(uint16_t expectedGeneration,
                                   Async::AsyncStatus status,
                                   std::span<const uint8_t> response) noexcept;

    /// Doorbell write completion handler.
    void OnDoorbellComplete(uint16_t expectedGeneration,
                            Async::AsyncStatus status,
                            std::span<const uint8_t> response) noexcept;

    /// Fetch agent reset completion handler.
    void OnAgentResetComplete(uint16_t expectedGeneration,
                              Async::AsyncStatus status,
                              std::span<const uint8_t> response) noexcept;

    /// Unsolicited status enable completion handler.
    void OnUnsolicitedStatusEnableComplete(uint16_t expectedGeneration,
                                           Async::AsyncStatus status,
                                           std::span<const uint8_t> response) noexcept;

    // Fetch agent state
    Async::FWAddress fetchAgentAddress_{};
    Async::FWAddress doorbellAddress_{};
    Async::AsyncHandle fetchAgentWriteHandle_{};
    bool fetchAgentWriteInUse_{false};

    // ORB chain state
    SBP2CommandORB* chainTailORB_{nullptr};
    SBP2CommandORB* activeFetchAgentORB_{nullptr};
    std::deque<SBP2CommandORB*> pendingImmediateORBs_;
    std::unordered_map<uint64_t, SBP2CommandORB*> outstandingORBs_;

    // Doorbell state
    Async::AsyncHandle doorbellWriteHandle_{};
    bool doorbellInProgress_{false};
    bool doorbellRingAgain_{false};

    // Fetch agent write data (8-byte BE ORB address)
    std::array<uint8_t, 8> fetchAgentWriteData_{};

    // Agent reset state
    Async::FWAddress agentResetAddress_{};
    Async::AsyncHandle agentResetWriteHandle_{};
    bool agentResetInProgress_{false};
    std::function<void(int)> agentResetCallback_;

    // Unsolicited status enable state
    Async::FWAddress unsolicitedStatusAddress_{};
    Async::AsyncHandle unsolicitedStatusWriteHandle_{};
    bool unsolicitedStatusRequested_{false};
};

} // namespace ASFW::Protocols::SBP2
