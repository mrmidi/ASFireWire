#include "SBP2LoginSession.hpp"
#include "SBP2DelayedDispatch.hpp"
#include "AddressSpaceManager.hpp"

#include "../../Async/Interfaces/IFireWireBus.hpp"
#include "../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../Common/FWCommon.hpp"

#include <algorithm>

namespace ASFW::Protocols::SBP2 {

using namespace ASFW::Protocols::SBP2::Wire;

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

SBP2LoginSession::SBP2LoginSession(Async::IFireWireBus& bus,
                                   Async::IFireWireBusInfo& busInfo,
                                   AddressSpaceManager& addrSpaceMgr)
    : bus_(bus)
    , busInfo_(busInfo)
    , addrSpaceMgr_(addrSpaceMgr) {}

SBP2LoginSession::~SBP2LoginSession() {
    CancelPendingTimer();
    ClearORBTracking(true);
    lifetimeToken_.reset();
    ReleaseOwnedTimeoutQueue();
    DeallocateResources();
}

void SBP2LoginSession::SetWorkQueue(IODispatchQueue* queue) noexcept {
    workQueue_ = queue;
    if (timeoutQueue_ == nullptr) {
        EnsureTimeoutQueue();
    }
}

void SBP2LoginSession::SetTimeoutQueue(IODispatchQueue* queue) noexcept {
    timeoutQueue_ = queue;
    if (queue != nullptr) {
        ReleaseOwnedTimeoutQueue();
    } else {
        EnsureTimeoutQueue();
    }
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void SBP2LoginSession::Configure(const SBP2TargetInfo& info) noexcept {
    targetInfo_ = info;
    configured_ = true;

    ASFW_LOG(SBP2,
             "SBP2LoginSession: configured target node=0x%04x mgmt_offset=%u LUN=%u "
             "mgmt_timeout=%ums max_orb=%u max_cmd_block=%u",
             info.targetNodeId,
             info.managementAgentOffset,
             info.lun,
             info.managementTimeoutMs,
             info.maxORBSize,
             info.maxCommandBlockSize);
}

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------

bool SBP2LoginSession::Login() noexcept {
    if (!configured_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::Login: not configured");
        return false;
    }

    if (state_ == LoginState::LoggingIn || state_ == LoginState::LoggedIn) {
        ASFW_LOG(SBP2, "SBP2LoginSession::Login: state=%s, ignoring", ToString(state_));
        return false;
    }

    // Allocate address spaces for ORB/response/status on first login.
    if (!AllocateResources()) {
        ASFW_LOG(SBP2, "SBP2LoginSession::Login: resource allocation failed");
        SetState(LoginState::Failed);
        return false;
    }

    SetState(LoginState::LoggingIn);
    loginGeneration_ = static_cast<uint16_t>(busInfo_.GetGeneration().value);
    loginNodeID_ = targetInfo_.targetNodeId;

    BuildLoginORB();

    ASFW_LOG(SBP2,
             "SBP2LoginSession::Login: sending login ORB to node 0x%04x gen=%u LUN=%u",
             loginNodeID_, loginGeneration_, targetInfo_.lun);

    // Write the Login ORB address to the management agent.
    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)};
    const Async::FWAddress mgmtAddr{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = 0xFFFF,
            .addressLo = ManagementAgentAddressLo(targetInfo_.managementAgentOffset),
            .nodeID = loginNodeID_
        }
    };
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    loginWriteHandle_ = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{loginORBAddressBE_.data(), loginORBAddressBE_.size()},
        speed,
        [this, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                      std::span<const uint8_t> response) {
            OnLoginWriteComplete(requestGeneration, status, response);
        });

    if (!loginWriteHandle_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::Login: WriteBlock failed immediately");
        SetState(LoginState::Failed);
        return false;
    }

    // Start management timeout
    StartLoginTimer();
    return true;
}

// ---------------------------------------------------------------------------
// Logout
// ---------------------------------------------------------------------------

bool SBP2LoginSession::Logout() noexcept {
    if (state_ != LoginState::LoggedIn && state_ != LoginState::Suspended) {
        ASFW_LOG(SBP2, "SBP2LoginSession::Logout: state=%s, ignoring", ToString(state_));
        return false;
    }

    SetState(LoginState::LoggingOut);
    BuildLogoutORB();

    ASFW_LOG(SBP2, "SBP2LoginSession::Logout: sending logout ORB loginID=%u", loginID_);

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)};
    const Async::FWAddress mgmtAddr{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = 0xFFFF,
            .addressLo = ManagementAgentAddressLo(targetInfo_.managementAgentOffset),
            .nodeID = loginNodeID_
        }
    };
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    logoutWriteHandle_ = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{logoutORBAddressBE_.data(), logoutORBAddressBE_.size()},
        speed,
        [this, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                      std::span<const uint8_t> response) {
            OnLogoutWriteComplete(requestGeneration, status, response);
        });

    if (!logoutWriteHandle_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::Logout: WriteBlock failed");
        SetState(LoginState::Failed);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Reconnect
// ---------------------------------------------------------------------------

bool SBP2LoginSession::Reconnect() noexcept {
    if (state_ != LoginState::Suspended && state_ != LoginState::LoggedIn) {
        ASFW_LOG(SBP2, "SBP2LoginSession::Reconnect: state=%s, ignoring", ToString(state_));
        return false;
    }

    SetState(LoginState::Reconnecting);
    loginGeneration_ = static_cast<uint16_t>(busInfo_.GetGeneration().value);

    BuildReconnectORB();

    ASFW_LOG(SBP2,
             "SBP2LoginSession::Reconnect: sending reconnect ORB loginID=%u gen=%u",
             loginID_, loginGeneration_);

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)};
    const Async::FWAddress mgmtAddr{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = 0xFFFF,
            .addressLo = ManagementAgentAddressLo(targetInfo_.managementAgentOffset),
            .nodeID = loginNodeID_
        }
    };
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    reconnectWriteHandle_ = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{reconnectORBAddressBE_.data(), reconnectORBAddressBE_.size()},
        speed,
        [this, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                      std::span<const uint8_t> response) {
            OnReconnectWriteComplete(requestGeneration, status, response);
        });

    if (!reconnectWriteHandle_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::Reconnect: WriteBlock failed, will retry");
        SubmitDelayedCallback(kLoginRetryDelayMs, [this]() { OnReconnectTimeout(); });
        reconnectTimerActive_ = true;
        return true; // Will retry
    }

    return true;
}

// ---------------------------------------------------------------------------
// Bus Reset Handling
// ---------------------------------------------------------------------------

void SBP2LoginSession::HandleBusReset(uint16_t newGeneration) noexcept {
    ASFW_LOG(SBP2,
             "SBP2LoginSession::HandleBusReset: state=%s newGen=%u loginGen=%u",
             ToString(state_), newGeneration, loginGeneration_);

    switch (state_) {
        case LoginState::LoggingIn:
            // Login was in progress — cancel and retry after reset settles.
            CancelLoginTimer();
            loginRetryCount_ = 0;
            loginGeneration_ = newGeneration;
            ClearORBTracking(true);
            SetState(LoginState::Idle);
            SubmitDelayedCallback(100, [this]() { (void)Login(); });
            break;

        case LoginState::LoggedIn:
            // Transition to Suspended — wait for topology then reconnect.
            CancelPendingTimer();
            ClearORBTracking(true);
            SetState(LoginState::Suspended);
            loginGeneration_ = newGeneration;
            break;

        case LoginState::Reconnecting:
            // Reconnect was in flight — retry.
            reconnectTimerActive_ = false;
            CancelPendingTimer();
            ClearORBTracking(true);
            loginGeneration_ = newGeneration;
            SetState(LoginState::Suspended);
            SubmitDelayedCallback(100, [this]() { (void)Reconnect(); });
            break;

        case LoginState::LoggingOut:
            // Logout in flight during bus reset — consider logged out.
            CancelPendingTimer();
            ClearORBTracking(true);
            SetState(LoginState::Idle);
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

Async::FWAddress SBP2LoginSession::CommandBlockAgent() const noexcept {
    return commandBlockAgent_;
}

uint32_t SBP2LoginSession::ReconnectHoldSeconds() const noexcept {
    return reconnectHold_ > 0 ? (1u << reconnectHold_) : 0;
}

// ---------------------------------------------------------------------------
// Resource Allocation
// ---------------------------------------------------------------------------

bool SBP2LoginSession::AllocateResources() noexcept {
    if (loginORBHandle_ != 0) {
        return true; // Already allocated
    }

    if (!AllocateLoginORBAddressSpace()) return false;
    if (!AllocateLoginResponseAddressSpace()) return false;
    if (!AllocateStatusBlockAddressSpace()) return false;
    if (!AllocateReconnectORBAddressSpace()) return false;
    if (!AllocateLogoutORBAddressSpace()) return false;

    // Register a callback for status block writes — the device writes status
    // here to signal login/reconnect/logout completion (and ORB completion
    // in Step 2).
    addrSpaceMgr_.SetRemoteWriteCallback(
        statusBlockHandle_,
        [this](uint64_t /*handle*/, uint32_t offset, std::span<const uint8_t> payload) {
            OnStatusBlockRemoteWrite(offset, payload);
        });

    ASFW_LOG(SBP2, "SBP2LoginSession: all address spaces allocated");
    return true;
}

void SBP2LoginSession::DeallocateResources() noexcept {
    ClearORBTracking(true);

    if (loginORBHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, loginORBHandle_);
        loginORBHandle_ = 0;
    }
    if (loginResponseHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, loginResponseHandle_);
        loginResponseHandle_ = 0;
    }
    if (statusBlockHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, statusBlockHandle_);
        statusBlockHandle_ = 0;
    }
    if (reconnectORBHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, reconnectORBHandle_);
        reconnectORBHandle_ = 0;
    }
    if (logoutORBHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, logoutORBHandle_);
        logoutORBHandle_ = 0;
    }
}

bool SBP2LoginSession::AllocateLoginORBAddressSpace() noexcept {
    // Login ORB is 32 bytes, readable by target device.
    auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::LoginORB::kSize,
        &loginORBHandle_, &loginORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate login ORB address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(loginORBHandle_, "sbp2-login-orb");
    return true;
}

bool SBP2LoginSession::AllocateLoginResponseAddressSpace() noexcept {
    // Login response is 16 bytes, writable by target device.
    auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::LoginResponse::kSize,
        &loginResponseHandle_, &loginResponseMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate login response address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(loginResponseHandle_, "sbp2-login-response");
    return true;
}

bool SBP2LoginSession::AllocateStatusBlockAddressSpace() noexcept {
    // Status block is up to 32 bytes, writable by target device.
    auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::StatusBlock::kMaxSize,
        &statusBlockHandle_, &statusBlockMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate status block address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(statusBlockHandle_, "sbp2-status-fifo");
    return true;
}

bool SBP2LoginSession::AllocateReconnectORBAddressSpace() noexcept {
    auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::ReconnectORB::kSize,
        &reconnectORBHandle_, &reconnectORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate reconnect ORB address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(reconnectORBHandle_, "sbp2-reconnect-orb");
    return true;
}

bool SBP2LoginSession::AllocateLogoutORBAddressSpace() noexcept {
    auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::LogoutORB::kSize,
        &logoutORBHandle_, &logoutORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate logout ORB address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(logoutORBHandle_, "sbp2-logout-orb");
    return true;
}

// ---------------------------------------------------------------------------
// ORB Construction
// ---------------------------------------------------------------------------

void SBP2LoginSession::BuildLoginORB() noexcept {
    std::memset(&loginORBBuffer_, 0, sizeof(loginORBBuffer_));

    // Get local node ID for filling address fields.
    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    // Login response address: nodeID in upper 16 bits of addressHi.
    const uint32_t responseAddrHi = ToBE32(
        ComposeBusAddressHi(localNode, loginResponseMeta_.addressHi));
    const uint32_t responseAddrLo = ToBE32(loginResponseMeta_.addressLo);

    // Status FIFO address.
    const uint32_t statusAddrHi = ToBE32(
        ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    const uint32_t statusAddrLo = ToBE32(statusBlockMeta_.addressLo);

    // Fill login ORB fields.
    loginORBBuffer_.loginResponseAddressHi = responseAddrHi;
    loginORBBuffer_.loginResponseAddressLo = responseAddrLo;
    loginORBBuffer_.options = static_cast<uint16_t>(
        Options::kLoginNotify | Options::kExclusiveLogin);
    loginORBBuffer_.lun = ToBE16(targetInfo_.lun);
    loginORBBuffer_.passwordLength = 0;
    loginORBBuffer_.loginResponseLength = ToBE16(sizeof(Wire::LoginResponse));
    loginORBBuffer_.statusFIFOAddressHi = statusAddrHi;
    loginORBBuffer_.statusFIFOAddressLo = statusAddrLo;

    // Write login ORB data to address space so device can read it.
    addrSpaceMgr_.WriteLocalData(
        this, loginORBHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&loginORBBuffer_),
                                  sizeof(loginORBBuffer_)});

    // Build the 8-byte management agent write payload: ORB address in big-endian.
    // Format: [nodeID(2)][addressHi(2)][addressLo(4)]
    loginORBAddressBE_[0] = static_cast<uint8_t>(localNode >> 8);
    loginORBAddressBE_[1] = static_cast<uint8_t>(localNode & 0xFF);
    loginORBAddressBE_[2] = static_cast<uint8_t>(loginORBMeta_.addressHi >> 8);
    loginORBAddressBE_[3] = static_cast<uint8_t>(loginORBMeta_.addressHi & 0xFF);
    const uint32_t orbAddrLoBE = ToBE32(loginORBMeta_.addressLo);
    std::memcpy(&loginORBAddressBE_[4], &orbAddrLoBE, sizeof(uint32_t));

    ASFW_LOG(SBP2,
             "SBP2LoginSession::BuildLoginORB: mgmt=0x%08x payload=%02x%02x:%02x%02x:%02x%02x%02x%02x "
             "ORB at %04x:%08x, response at %04x:%08x, status at %04x:%08x, LUN=%u",
             ManagementAgentAddressLo(targetInfo_.managementAgentOffset),
             loginORBAddressBE_[0], loginORBAddressBE_[1],
             loginORBAddressBE_[2], loginORBAddressBE_[3],
             loginORBAddressBE_[4], loginORBAddressBE_[5],
             loginORBAddressBE_[6], loginORBAddressBE_[7],
             localNode, loginORBMeta_.addressLo,
             localNode, loginResponseMeta_.addressLo,
             localNode, statusBlockMeta_.addressLo,
             targetInfo_.lun);
}

void SBP2LoginSession::BuildReconnectORB() noexcept {
    std::memset(&reconnectORBBuffer_, 0, sizeof(reconnectORBBuffer_));

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    // Reconnect ORB: options = reconnect (3) | notify
    reconnectORBBuffer_.options = Options::kReconnectNotify;
    reconnectORBBuffer_.loginID = ToBE16(loginID_);

    // Status FIFO address — reuse the dedicated status block address space.
    const uint32_t statusAddrHi = ToBE32(
        ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    const uint32_t statusAddrLo = ToBE32(statusBlockMeta_.addressLo);
    reconnectORBBuffer_.statusFIFOAddressHi = statusAddrHi;
    reconnectORBBuffer_.statusFIFOAddressLo = statusAddrLo;

    // Write reconnect ORB data.
    addrSpaceMgr_.WriteLocalData(
        this, reconnectORBHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&reconnectORBBuffer_),
                                  sizeof(reconnectORBBuffer_)});

    // Build management agent write payload.
    reconnectORBAddressBE_[0] = static_cast<uint8_t>(localNode >> 8);
    reconnectORBAddressBE_[1] = static_cast<uint8_t>(localNode & 0xFF);
    reconnectORBAddressBE_[2] = static_cast<uint8_t>(reconnectORBMeta_.addressHi >> 8);
    reconnectORBAddressBE_[3] = static_cast<uint8_t>(reconnectORBMeta_.addressHi & 0xFF);
    const uint32_t addrLoBE = ToBE32(reconnectORBMeta_.addressLo);
    std::memcpy(&reconnectORBAddressBE_[4], &addrLoBE, sizeof(uint32_t));
}

void SBP2LoginSession::BuildLogoutORB() noexcept {
    std::memset(&logoutORBBuffer_, 0, sizeof(logoutORBBuffer_));

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    logoutORBBuffer_.options = Options::kLogoutNotify;
    logoutORBBuffer_.loginID = ToBE16(loginID_);

    // Status FIFO address — reuse the dedicated status block address space.
    const uint32_t statusAddrHi = ToBE32(
        ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    const uint32_t statusAddrLo = ToBE32(statusBlockMeta_.addressLo);
    logoutORBBuffer_.statusFIFOAddressHi = statusAddrHi;
    logoutORBBuffer_.statusFIFOAddressLo = statusAddrLo;

    addrSpaceMgr_.WriteLocalData(
        this, logoutORBHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&logoutORBBuffer_),
                                  sizeof(logoutORBBuffer_)});

    logoutORBAddressBE_[0] = static_cast<uint8_t>(localNode >> 8);
    logoutORBAddressBE_[1] = static_cast<uint8_t>(localNode & 0xFF);
    logoutORBAddressBE_[2] = static_cast<uint8_t>(logoutORBMeta_.addressHi >> 8);
    logoutORBAddressBE_[3] = static_cast<uint8_t>(logoutORBMeta_.addressHi & 0xFF);
    const uint32_t addrLoBE = ToBE32(logoutORBMeta_.addressLo);
    std::memcpy(&logoutORBAddressBE_[4], &addrLoBE, sizeof(uint32_t));
}

// ---------------------------------------------------------------------------
// Completion Handlers
// ---------------------------------------------------------------------------

void SBP2LoginSession::OnLoginWriteComplete(uint16_t expectedGeneration,
                                            Async::AsyncStatus status,
                                            std::span<const uint8_t> response) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::LoggingIn) {
        return;
    }

    CancelLoginTimer();

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession::OnLoginWriteComplete: status=%s, retrying (%u/%u)",
                 Async::ToString(status), loginRetryCount_ + 1, kLoginRetryMax);

        if (loginRetryCount_ < kLoginRetryMax) {
            loginRetryCount_++;
            SubmitDelayedCallback(kLoginRetryDelayMs, [this]() {
                loginGeneration_ = static_cast<uint16_t>(busInfo_.GetGeneration().value);
                loginNodeID_ = targetInfo_.targetNodeId;
                SetState(LoginState::Idle);
                (void)Login();
            });
            return;
        }

        ASFW_LOG(SBP2, "SBP2LoginSession: login retries exhausted");
        SetState(LoginState::Failed);

        if (loginCallback_) {
            LoginCompleteParams params{};
            params.status = -1;
            params.generation = loginGeneration_;
            loginCallback_(params);
        }
        return;
    }

    // Management agent write ACK'd. The device will now:
    //   1. Fetch the ORB via read from our address space
    //   2. Process the login
    //   3. Write login response to our address space
    //   4. Write status block to our status FIFO
    //
    // We wait for the status block write callback (OnStatusBlockRemoteWrite)
    // before reading the login response. Restart the timer for the device
    // processing window.
    ASFW_LOG(SBP2, "SBP2LoginSession: management agent write ACK'd, waiting for status block");
    StartLoginTimer();
}

void SBP2LoginSession::OnLoginTimeout() noexcept {
    loginTimerActive_ = false;

    if (state_ != LoginState::LoggingIn) {
        return; // Already handled
    }

    ASFW_LOG(SBP2,
             "SBP2LoginSession: login timeout (%u/%u) waiting for target node=0x%04x "
             "to read label=sbp2-login-orb at %04x:%08x and write label=sbp2-status-fifo at %04x:%08x",
             loginRetryCount_ + 1,
             kLoginRetryMax,
             loginNodeID_,
             loginORBMeta_.addressHi,
             loginORBMeta_.addressLo,
             statusBlockMeta_.addressHi,
             statusBlockMeta_.addressLo);

    if (loginRetryCount_ < kLoginRetryMax) {
        loginRetryCount_++;
        SetState(LoginState::Idle);
        (void)Login();
    } else {
        SetState(LoginState::Failed);
        if (loginCallback_) {
            LoginCompleteParams params{};
            params.status = -2; // timeout
            params.generation = loginGeneration_;
            loginCallback_(params);
        }
    }
}

void SBP2LoginSession::OnReconnectWriteComplete(uint16_t expectedGeneration,
                                                Async::AsyncStatus status,
                                                std::span<const uint8_t> response) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::Reconnecting) {
        return;
    }

    reconnectTimerActive_ = false;

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession::OnReconnectWriteComplete: status=%s, retrying",
                 Async::ToString(status));

        SubmitDelayedCallback(100, [this]() { (void)Reconnect(); });
        return;
    }

    // Reconnect ORB write ACK'd. Wait for status block from device.
    ASFW_LOG(SBP2, "SBP2LoginSession: reconnect write ACK'd, waiting for status block");
}

void SBP2LoginSession::OnReconnectTimeout() noexcept {
    reconnectTimerActive_ = false;

    if (state_ != LoginState::Reconnecting) {
        return;
    }

    ASFW_LOG(SBP2, "SBP2LoginSession: reconnect timeout, falling back to full login");
    SetState(LoginState::Idle);
    (void)Login();
}

void SBP2LoginSession::OnLogoutWriteComplete(uint16_t expectedGeneration,
                                             Async::AsyncStatus status,
                                             std::span<const uint8_t> response) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::LoggingOut) {
        return;
    }

    logoutTimerActive_ = false;

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession::OnLogoutWriteComplete: status=%s",
                 Async::ToString(status));
    }

    const uint16_t oldLoginID = loginID_;
    loginID_ = 0;
    SetState(LoginState::Idle);

    ASFW_LOG(SBP2, "SBP2LoginSession: logout complete (was loginID=%u)", oldLoginID);

    if (logoutCallback_) {
        LogoutCompleteParams params{};
        params.status = (status == Async::AsyncStatus::kSuccess) ? 0 : -1;
        params.generation = loginGeneration_;
        logoutCallback_(params);
    }
}

void SBP2LoginSession::OnLogoutTimeout() noexcept {
    logoutTimerActive_ = false;
    ASFW_LOG(SBP2, "SBP2LoginSession: logout timeout, transitioning to Idle anyway");
    loginID_ = 0;
    SetState(LoginState::Idle);

    if (logoutCallback_) {
        LogoutCompleteParams params{};
        params.status = -2;
        params.generation = loginGeneration_;
        logoutCallback_(params);
    }
}

// ---------------------------------------------------------------------------
// Status Block Handling
// ---------------------------------------------------------------------------

void SBP2LoginSession::OnStatusBlockRemoteWrite(uint32_t offset,
                                                 std::span<const uint8_t> payload) noexcept {
    if (payload.empty()) {
        return;
    }

    Wire::StatusBlock block{};
    uint32_t len = static_cast<uint32_t>(payload.size());
    if (len > sizeof(block)) {
        len = sizeof(block);
    }
    std::memcpy(&block, payload.data(), len);

    ASFW_LOG(SBP2,
             "SBP2LoginSession::OnStatusBlockRemoteWrite: state=%s offset=%u len=%u "
             "src=%u resp=%u dead=%u sbpStatus=%u",
             ToString(state_), offset, len,
             block.Source(), block.Response(), block.DeadBit(), block.sbpStatus);

    // Dispatch to state-specific handler.
    switch (state_) {
        case LoginState::LoggingIn:
            CancelLoginTimer();
            CompleteLoginFromStatusBlock(block, len);
            break;

        case LoginState::Reconnecting:
            reconnectTimerActive_ = false;
            CompleteReconnectFromStatusBlock(block, len);
            break;

        case LoginState::LoggingOut:
            logoutTimerActive_ = false;
            CompleteLogoutFromStatusBlock(block, len);
            break;

        case LoginState::LoggedIn:
            // Unsolicited status or ORB completion — forward to callback.
            ProcessStatusBlock(block, len);
            break;

        default:
            ASFW_LOG(SBP2, "SBP2LoginSession: unexpected status block in state %s", ToString(state_));
            break;
    }
}

void SBP2LoginSession::CompleteLoginFromStatusBlock(const Wire::StatusBlock& block,
                                                     uint32_t length) noexcept {
    if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        ASFW_LOG(SBP2,
                 "SBP2LoginSession: login failed — sbpStatus=%u, retrying (%u/%u)",
                 block.sbpStatus, loginRetryCount_ + 1, kLoginRetryMax);

        if (loginRetryCount_ < kLoginRetryMax) {
            loginRetryCount_++;
            SubmitDelayedCallback(kLoginRetryDelayMs, [this]() {
                loginGeneration_ = static_cast<uint16_t>(busInfo_.GetGeneration().value);
                loginNodeID_ = targetInfo_.targetNodeId;
                SetState(LoginState::Idle);
                (void)Login();
            });
            return;
        }

        SetState(LoginState::Failed);
        if (loginCallback_) {
            LoginCompleteParams params{};
            params.status = -1;
            params.statusBlock = block;
            params.statusBlockLength = length;
            params.generation = loginGeneration_;
            loginCallback_(params);
        }
        return;
    }

    // Login succeeded — read the login response that the device wrote to
    // our address space.
    std::vector<uint8_t> responseData;
    auto kr = addrSpaceMgr_.ReadIncomingData(
        this, loginResponseHandle_, 0, sizeof(Wire::LoginResponse), &responseData);

    if (kr != kIOReturnSuccess || responseData.size() < sizeof(Wire::LoginResponse)) {
        ASFW_LOG(SBP2,
                 "SBP2LoginSession: failed to read login response (kr=0x%08x, len=%zu)",
                 kr, responseData.size());

        if (loginRetryCount_ < kLoginRetryMax) {
            loginRetryCount_++;
            SubmitDelayedCallback(kLoginRetryDelayMs, [this]() {
                loginGeneration_ = static_cast<uint16_t>(busInfo_.GetGeneration().value);
                loginNodeID_ = targetInfo_.targetNodeId;
                SetState(LoginState::Idle);
                (void)Login();
            });
            return;
        }

        SetState(LoginState::Failed);
        if (loginCallback_) {
            LoginCompleteParams params{};
            params.status = -1;
            params.statusBlock = block;
            params.statusBlockLength = length;
            params.generation = loginGeneration_;
            loginCallback_(params);
        }
        return;
    }

    // Parse login response.
    Wire::LoginResponse resp{};
    std::memcpy(&resp, responseData.data(), sizeof(resp));

    loginID_ = FromBE16(resp.loginID);
    reconnectHold_ = FromBE16(resp.reconnectHold);
    loginResponse_ = resp;

    // Extract command block agent address.
    const uint32_t cbaHi = FromBE32(resp.commandBlockAgentAddressHi);
    const uint32_t cbaLo = FromBE32(resp.commandBlockAgentAddressLo);
    commandBlockAgent_ = Async::FWAddress{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = static_cast<uint16_t>(cbaHi & 0xFFFFu),
            .addressLo = cbaLo,
            .nodeID = loginNodeID_
        }
    };

    loginRetryCount_ = 0;
    SetState(LoginState::LoggedIn);

    // If unsolicited status was requested while not logged in, enable it now.
    if (unsolicitedStatusRequested_) {
        unsolicitedStatusRequested_ = false;
        EnableUnsolicitedStatus();
    }

    // Compute CBA-derived addresses for fetch agent, doorbell, agent reset,
    // and unsolicited status enable.
    fetchAgentAddress_ = Async::FWAddress{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = commandBlockAgent_.addressHi,
            .addressLo = commandBlockAgent_.addressLo + Wire::CommandBlockAgentOffsets::kFetchAgent,
            .nodeID = loginNodeID_
        }
    };
    doorbellAddress_ = Async::FWAddress{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = commandBlockAgent_.addressHi,
            .addressLo = commandBlockAgent_.addressLo + Wire::CommandBlockAgentOffsets::kDoorbell,
            .nodeID = loginNodeID_
        }
    };
    agentResetAddress_ = Async::FWAddress{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = commandBlockAgent_.addressHi,
            .addressLo = commandBlockAgent_.addressLo + Wire::CommandBlockAgentOffsets::kAgentReset,
            .nodeID = loginNodeID_
        }
    };
    unsolicitedStatusAddress_ = Async::FWAddress{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = commandBlockAgent_.addressHi,
            .addressLo = commandBlockAgent_.addressLo + Wire::CommandBlockAgentOffsets::kUnsolicitedStatusEnable,
            .nodeID = loginNodeID_
        }
    };

    ASFW_LOG(SBP2,
             "SBP2LoginSession: login successful — loginID=%u, CBA=%04x:%08x, "
             "reconnectHold=2^%u=%us",
             loginID_,
             commandBlockAgent_.addressHi, commandBlockAgent_.addressLo,
             reconnectHold_, ReconnectHoldSeconds());

    // Send Set Busy Timeout to the device.
    {
        const uint32_t busyTimeout = ToBE32(kBusyTimeoutValue);
        const Async::FWAddress busyAddr{
            Async::FWAddress::QualifiedAddressParts{
                .addressHi = kCSRBusAddressHi,
                .addressLo = kBusyTimeoutAddressLo,
                .nodeID = loginNodeID_
            }
        };
        bus_.WriteBlock(
            FW::Generation{loginGeneration_},
            FW::NodeId{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)},
            busyAddr,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&busyTimeout), 4},
            busInfo_.GetSpeed(FW::NodeId{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)}),
            [](Async::AsyncStatus, std::span<const uint8_t>) {});
    }

    if (loginCallback_) {
        LoginCompleteParams params{};
        params.status = 0;
        params.loginResponse = loginResponse_;
        params.statusBlock = block;
        params.statusBlockLength = length;
        params.generation = loginGeneration_;
        loginCallback_(params);
    }
}

void SBP2LoginSession::CompleteReconnectFromStatusBlock(const Wire::StatusBlock& block,
                                                         uint32_t length) noexcept {
    if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        ASFW_LOG(SBP2,
                 "SBP2LoginSession: reconnect failed — sbpStatus=%u, falling back to full login",
                 block.sbpStatus);

        SetState(LoginState::Idle);
        (void)Login();
        return;
    }

    SetState(LoginState::LoggedIn);
    ASFW_LOG(SBP2, "SBP2LoginSession: reconnect successful — loginID=%u", loginID_);

    // If unsolicited status was requested while not logged in, enable it now.
    if (unsolicitedStatusRequested_) {
        unsolicitedStatusRequested_ = false;
        EnableUnsolicitedStatus();
    }

    if (loginCallback_) {
        LoginCompleteParams params{};
        params.status = 0;
        params.loginResponse = loginResponse_;
        params.statusBlock = block;
        params.statusBlockLength = length;
        params.generation = loginGeneration_;
        loginCallback_(params);
    }
}

void SBP2LoginSession::CompleteLogoutFromStatusBlock(const Wire::StatusBlock& block,
                                                      uint32_t length) noexcept {
    const uint16_t oldLoginID = loginID_;
    loginID_ = 0;
    SetState(LoginState::Idle);

    ASFW_LOG(SBP2, "SBP2LoginSession: logout complete (was loginID=%u)", oldLoginID);

    if (logoutCallback_) {
        LogoutCompleteParams params{};
        params.status = 0;
        params.generation = loginGeneration_;
        logoutCallback_(params);
    }
}

void SBP2LoginSession::ProcessStatusBlock(const Wire::StatusBlock& block,
                                           uint32_t length) noexcept {
    // Distinguish unsolicited vs solicited status.
    // Unsolicited: (details & 0xC0) == 0x80 (source bit set, resp == 0)
    const bool isUnsolicited = (block.details & 0xC0) == 0x80;

    if (statusCallback_) {
        statusCallback_(block, length);
    }

    if (isUnsolicited) {
        // Re-enable unsolicited status so device can send more
        EnableUnsolicitedStatus();
        return;
    }

    const uint64_t orbKey = MakeORBKey(FromBE16(block.orbOffsetHi), FromBE32(block.orbOffsetLo));
    const auto it = outstandingORBs_.find(orbKey);
    if (it == outstandingORBs_.end()) {
        ASFW_LOG(SBP2,
                 "SBP2LoginSession::ProcessStatusBlock: unmatched ORB status hi=%04x lo=%08x",
                 FromBE16(block.orbOffsetHi),
                 FromBE32(block.orbOffsetLo));
        return;
    }

    SBP2CommandORB* orb = it->second;
    outstandingORBs_.erase(it);
    if (orb != nullptr) {
        orb->CancelTimer();
        auto& cb = orb->GetCompletionCallback();
        if (cb) {
            cb(0, block.sbpStatus);
        }
    }
}

// ---------------------------------------------------------------------------
// Internal Helpers
// ---------------------------------------------------------------------------

void SBP2LoginSession::SetState(LoginState newState) noexcept {
    if (state_ != newState) {
        ASFW_LOG(SBP2, "SBP2LoginSession: state %s -> %s", ToString(state_), ToString(newState));
        state_ = newState;
    }
}

void SBP2LoginSession::StartLoginTimer() noexcept {
    loginTimerActive_ = true;
    SubmitDelayedCallback(targetInfo_.managementTimeoutMs, [this]() {
        OnLoginTimeout();
    });
}

void SBP2LoginSession::CancelLoginTimer() noexcept {
    loginTimerActive_ = false;
    CancelPendingTimer();
}

void SBP2LoginSession::CancelPendingTimer() noexcept {
    delayedCallbackGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

void SBP2LoginSession::EnsureTimeoutQueue() noexcept {
    if (timeoutQueue_ != nullptr || workQueue_ == nullptr) {
        return;
    }

#ifdef ASFW_HOST_TEST
    ownedTimeoutQueue_ = std::make_unique<IODispatchQueue>();
    if (ownedTimeoutQueue_ != nullptr &&
        workQueue_->UsesManualDispatchForTesting()) {
        ownedTimeoutQueue_->SetManualDispatchForTesting(true);
    }
    timeoutQueue_ = ownedTimeoutQueue_.get();
#else
    IODispatchQueue* queue = nullptr;
    const kern_return_t kr = IODispatchQueue::Create("com.asfw.sbp2.timeout", 0, 0, &queue);
    if (kr != kIOReturnSuccess || queue == nullptr) {
        ASFW_LOG(SBP2,
                 "SBP2LoginSession: failed to create timeout queue (kr=0x%08x), falling back to workQueue",
                 kr);
        timeoutQueue_ = workQueue_;
        return;
    }
    ownedTimeoutQueue_ = queue;
    timeoutQueue_ = ownedTimeoutQueue_;
#endif
}

void SBP2LoginSession::ReleaseOwnedTimeoutQueue() noexcept {
#ifdef ASFW_HOST_TEST
    IODispatchQueue* ownedQueue = ownedTimeoutQueue_.get();
    ownedTimeoutQueue_.reset();
    if (timeoutQueue_ == ownedQueue) {
        timeoutQueue_ = nullptr;
    }
#else
    IODispatchQueue* ownedQueue = ownedTimeoutQueue_;
    if (ownedTimeoutQueue_ != nullptr) {
        ownedTimeoutQueue_->release();
        ownedTimeoutQueue_ = nullptr;
    }
    if (timeoutQueue_ == ownedQueue) {
        timeoutQueue_ = nullptr;
    }
#endif
}

IODispatchQueue* SBP2LoginSession::EffectiveTimeoutQueue() const noexcept {
    if (timeoutQueue_ != nullptr) {
        return timeoutQueue_;
    }
    return workQueue_;
}

void SBP2LoginSession::SubmitDelayedCallback(uint64_t delayMs,
                                              std::function<void()> callback) noexcept {
    IODispatchQueue* delayQueue = timeoutQueue_ != nullptr ? timeoutQueue_ : workQueue_;
    if (workQueue_ == nullptr || delayQueue == nullptr || !callback) {
        return;
    }

    const uint64_t expectedGeneration =
        delayedCallbackGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1ULL;
    const std::weak_ptr<int> weakLifetime = lifetimeToken_;
    const uint64_t delayNs = delayMs * 1'000'000ULL;
    IODispatchQueue* bounceQueue = workQueue_;

    DispatchAfterCompat(delayQueue,
                        delayNs,
                        [this,
                         weakLifetime,
                         expectedGeneration,
                         bounceQueue,
                         cb = std::move(callback)]() mutable {
        if (weakLifetime.expired()) {
            return;
        }
        DispatchAsyncCompat(bounceQueue,
                            [this,
                             weakLifetime,
                             expectedGeneration,
                             cb = std::move(cb)]() mutable {
            if (weakLifetime.expired()) {
                return;
            }
            if (delayedCallbackGeneration_.load(std::memory_order_acquire) != expectedGeneration) {
                return;
            }
            cb();
        });
    });
}

uint64_t SBP2LoginSession::MakeORBKey(uint16_t addressHi, uint32_t addressLo) noexcept {
    return (static_cast<uint64_t>(addressHi) << 32) | static_cast<uint64_t>(addressLo);
}

uint64_t SBP2LoginSession::MakeORBKey(const Async::FWAddress& address) noexcept {
    return MakeORBKey(address.addressHi, address.addressLo);
}

void SBP2LoginSession::ClearORBTracking(bool cancelTimers) noexcept {
    if (cancelTimers) {
        for (auto& [key, orb] : outstandingORBs_) {
            if (orb != nullptr) {
                orb->CancelTimer();
                orb->SetAppended(false);
            }
        }
        if (activeFetchAgentORB_ != nullptr) {
            activeFetchAgentORB_->CancelTimer();
        }
        for (auto* orb : pendingImmediateORBs_) {
            if (orb != nullptr) {
                orb->CancelTimer();
            }
        }
    }

    outstandingORBs_.clear();
    pendingImmediateORBs_.clear();
    chainTailORB_ = nullptr;
    activeFetchAgentORB_ = nullptr;
    fetchAgentWriteHandle_ = {};
    fetchAgentWriteInUse_ = false;
    doorbellWriteHandle_ = {};
    doorbellInProgress_ = false;
    doorbellRingAgain_ = false;
}

// ---------------------------------------------------------------------------
// ORB Submission
// ---------------------------------------------------------------------------

bool SBP2LoginSession::SubmitORB(SBP2CommandORB* orb) noexcept {
    if (state_ != LoginState::LoggedIn) {
        ASFW_LOG(SBP2, "SBP2LoginSession::SubmitORB: state=%s, rejecting", ToString(state_));
        return false;
    }

    if (orb == nullptr || !orb->IsValid() || orb->IsAppended()) {
        ASFW_LOG(SBP2, "SBP2LoginSession::SubmitORB: invalid ORB (null=%d, valid=%d, appended=%d)",
                 orb == nullptr,
                 orb != nullptr && orb->IsValid(),
                 orb != nullptr && orb->IsAppended());
        return false;
    }

    // Fetch agent and doorbell addresses are computed at login time.

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));
    const FW::FwSpeed speed = busInfo_.GetSpeed(
        FW::NodeId{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)});

    // Max payload log: derive from maxPayloadSize_ (bytes → log2(quadlets)).
    // Capped at 15 (max 2^15 = 32768 quadlets = 128KB).
    uint16_t maxPayloadLog = 0;
    {
        uint16_t payloadBytes = maxPayloadSize_;
        if (payloadBytes > 4096) payloadBytes = 4096;
        uint16_t quadlets = payloadBytes / 4;
        if (quadlets > 0) {
            maxPayloadLog = 0;
            while ((1u << maxPayloadLog) < quadlets && maxPayloadLog < 15) {
                maxPayloadLog++;
            }
        }
    }

    const kern_return_t prepareKr = orb->PrepareForExecution(localNode, speed, maxPayloadLog);
    if (prepareKr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession::SubmitORB: PrepareForExecution failed: 0x%08x",
                 prepareKr);
        return false;
    }

    orb->SetFetchAgentWriteRetries(20);
    orb->SetAppended(true);
    outstandingORBs_[MakeORBKey(orb->GetORBAddress())] = orb;

    const bool isImmediate = (orb->GetFlags() & SBP2CommandORB::kImmediate) != 0;

    if (isImmediate) {
        chainTailORB_ = orb;

        if (fetchAgentWriteInUse_) {
            pendingImmediateORBs_.push_back(orb);
            ASFW_LOG(SBP2, "SBP2LoginSession::SubmitORB: fetch agent busy, deferring ORB");
            return true;
        }

        return AppendORBImmediate(orb);
    } else {
        // Chained: link to last ORB, ring doorbell
        if (!AppendORB(orb)) {
            return false;
        }
        RingDoorbell();
    }

    return true;
}

bool SBP2LoginSession::AppendORBImmediate(SBP2CommandORB* orb) noexcept {
    if (orb == nullptr || fetchAgentWriteInUse_) {
        return false;
    }

    // Build 8-byte ORB address in big-endian
    // Format: [nodeID(2)][addressHi(2)][addressLo(4)]
    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));
    const Async::FWAddress orbAddr = orb->GetORBAddress();

    fetchAgentWriteData_[0] = static_cast<uint8_t>(localNode >> 8);
    fetchAgentWriteData_[1] = static_cast<uint8_t>(localNode & 0xFF);
    fetchAgentWriteData_[2] = static_cast<uint8_t>(orbAddr.addressHi >> 8);
    fetchAgentWriteData_[3] = static_cast<uint8_t>(orbAddr.addressHi & 0xFF);
    const uint32_t addrLoBE = ToBE32(orbAddr.addressLo);
    std::memcpy(&fetchAgentWriteData_[4], &addrLoBE, sizeof(uint32_t));

    activeFetchAgentORB_ = orb;
    fetchAgentWriteInUse_ = true;

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    fetchAgentWriteHandle_ = bus_.WriteBlock(
        gen, node, fetchAgentAddress_,
        std::span<const uint8_t>{fetchAgentWriteData_.data(), fetchAgentWriteData_.size()},
        speed,
        [this, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                     std::span<const uint8_t> response) {
            OnFetchAgentWriteComplete(requestGeneration, status, response);
        });

    if (!fetchAgentWriteHandle_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::AppendORBImmediate: WriteBlock failed");
        fetchAgentWriteInUse_ = false;
        activeFetchAgentORB_ = nullptr;
        FailSubmittedORB(orb, -1, Wire::SBPStatus::kUnspecifiedError);
        return false;
    }

    ASFW_LOG(SBP2,
             "SBP2LoginSession::AppendORBImmediate: wrote ORB addr %04x:%08x to fetch agent",
             localNode, orbAddr.addressLo);
    return true;
}

void SBP2LoginSession::FailSubmittedORB(SBP2CommandORB* orb,
                                        int transportStatus,
                                        uint8_t sbpStatus) noexcept {
    if (orb == nullptr) {
        return;
    }

    const auto key = MakeORBKey(orb->GetORBAddress());
    outstandingORBs_.erase(key);
    pendingImmediateORBs_.erase(
        std::remove(pendingImmediateORBs_.begin(), pendingImmediateORBs_.end(), orb),
        pendingImmediateORBs_.end());
    if (activeFetchAgentORB_ == orb) {
        activeFetchAgentORB_ = nullptr;
    }
    if (chainTailORB_ == orb) {
        chainTailORB_ = nullptr;
    }
    orb->CancelTimer();
    orb->SetAppended(false);

    auto& cb = orb->GetCompletionCallback();
    if (cb) {
        cb(transportStatus, sbpStatus);
    }
}

bool SBP2LoginSession::AppendORB(SBP2CommandORB* orb) noexcept {
    if (chainTailORB_ == nullptr) {
        // First ORB — write directly to fetch agent instead of chaining
        chainTailORB_ = orb;
        return AppendORBImmediate(orb);
    }

    if (chainTailORB_ != orb) {
        const Async::FWAddress orbAddr = orb->GetORBAddress();

        // Set the new ORB's address in big-endian into the last ORB's next pointer
        const uint16_t localNode =
            NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));
        const uint32_t nextHi = ToBE32(ComposeBusAddressHi(localNode, orbAddr.addressHi));
        const uint32_t nextLo = ToBE32(orbAddr.addressLo);
        const kern_return_t linkKr = chainTailORB_->SetNextORBAddress(nextHi, nextLo);
        if (linkKr != kIOReturnSuccess) {
            ASFW_LOG(SBP2, "SBP2LoginSession::AppendORB: SetNextORBAddress failed: 0x%08x",
                     linkKr);
            FailSubmittedORB(orb, -1, Wire::SBPStatus::kUnspecifiedError);
            return false;
        }

        chainTailORB_ = orb;
    }

    return true;
}

void SBP2LoginSession::RingDoorbell() noexcept {
    if (doorbellInProgress_) {
        doorbellRingAgain_ = true;
        return;
    }

    doorbellInProgress_ = true;

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    doorbellWriteHandle_ = bus_.WriteQuad(
        gen, node, doorbellAddress_, 0, speed,
        [this, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                     std::span<const uint8_t> response) {
            OnDoorbellComplete(requestGeneration, status, response);
        });

    if (!doorbellWriteHandle_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::RingDoorbell: WriteQuad failed");
        doorbellInProgress_ = false;
    }
}

void SBP2LoginSession::OnFetchAgentWriteComplete(uint16_t expectedGeneration,
                                                 Async::AsyncStatus status,
                                                 std::span<const uint8_t> response) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::LoggedIn) {
        return;
    }

    fetchAgentWriteInUse_ = false;
    fetchAgentWriteHandle_ = {};

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2,
                 "SBP2LoginSession::OnFetchAgentWriteComplete: status=%s, retries=%u",
                 Async::ToString(status),
                 activeFetchAgentORB_ ? activeFetchAgentORB_->GetFetchAgentWriteRetries() : 0);

        if (activeFetchAgentORB_ != nullptr) {
            uint32_t retries = activeFetchAgentORB_->GetFetchAgentWriteRetries();
            if (retries > 0) {
                retries--;
                activeFetchAgentORB_->SetFetchAgentWriteRetries(retries);
                // Retry after a delay
                SBP2CommandORB* retryORB = activeFetchAgentORB_;
                SubmitDelayedCallback(1000, [this, retryORB]() {
                    if (activeFetchAgentORB_ == retryORB) {
                        AppendORBImmediate(retryORB);
                    }
                });
                return;
            }

            // Retries exhausted — report failure
            outstandingORBs_.erase(MakeORBKey(activeFetchAgentORB_->GetORBAddress()));
            activeFetchAgentORB_->SetAppended(false);
            auto& cb = activeFetchAgentORB_->GetCompletionCallback();
            if (cb) {
                cb(-1, Wire::SBPStatus::kUnspecifiedError);
            }
        }
        activeFetchAgentORB_ = nullptr;
        if (!pendingImmediateORBs_.empty()) {
            SBP2CommandORB* next = pendingImmediateORBs_.front();
            pendingImmediateORBs_.pop_front();
            AppendORBImmediate(next);
        }
        return;
    }

    // Fetch agent write succeeded. Submit deferred ORB if any.
    activeFetchAgentORB_ = nullptr;

    if (!pendingImmediateORBs_.empty()) {
        SBP2CommandORB* next = pendingImmediateORBs_.front();
        pendingImmediateORBs_.pop_front();
        ASFW_LOG(SBP2, "SBP2LoginSession: submitting deferred ORB");
        AppendORBImmediate(next);
    }
}

void SBP2LoginSession::OnDoorbellComplete(uint16_t expectedGeneration,
                                          Async::AsyncStatus status,
                                          std::span<const uint8_t> response) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::LoggedIn) {
        return;
    }

    doorbellInProgress_ = false;
    doorbellWriteHandle_ = {};

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession::OnDoorbellComplete: status=%s",
                 Async::ToString(status));
    }

    if (doorbellRingAgain_) {
        doorbellRingAgain_ = false;
        RingDoorbell();
    }
}

// ---------------------------------------------------------------------------
// Management ORB Submission
// ---------------------------------------------------------------------------

bool SBP2LoginSession::SubmitManagementORB(SBP2ManagementORB* orb) noexcept {
    if (state_ != LoginState::LoggedIn) {
        ASFW_LOG(SBP2, "SBP2LoginSession::SubmitManagementORB: state=%s, rejecting",
                 ToString(state_));
        return false;
    }

    if (orb == nullptr) {
        return false;
    }

    EnsureTimeoutQueue();

    // Configure the management ORB with current session parameters
    orb->SetLoginID(loginID_);
    orb->SetManagementAgentOffset(targetInfo_.managementAgentOffset);
    orb->SetTargetNode(loginGeneration_, loginNodeID_);
    orb->SetTimeout(targetInfo_.managementTimeoutMs);
    orb->SetWorkQueue(workQueue_);
    orb->SetTimeoutQueue(EffectiveTimeoutQueue());

    ASFW_LOG(SBP2, "SBP2LoginSession::SubmitManagementORB: function=%u",
             static_cast<uint16_t>(orb->GetFunction()));

    return orb->Execute();
}

// ---------------------------------------------------------------------------
// Fetch Agent Reset
// ---------------------------------------------------------------------------

void SBP2LoginSession::ResetFetchAgent(std::function<void(int)> callback) noexcept {
    if (state_ != LoginState::LoggedIn) {
        if (callback) callback(-1);
        return;
    }

    if (agentResetInProgress_) {
        if (callback) callback(-1);
        return;
    }

    agentResetInProgress_ = true;
    agentResetCallback_ = std::move(callback);

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    agentResetWriteHandle_ = bus_.WriteQuad(
        gen, node, agentResetAddress_, 0, speed,
        [this, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                     std::span<const uint8_t> response) {
            OnAgentResetComplete(requestGeneration, status, response);
        });

    if (!agentResetWriteHandle_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::ResetFetchAgent: WriteQuad failed");
        agentResetInProgress_ = false;
        if (agentResetCallback_) {
            agentResetCallback_(-1);
            agentResetCallback_ = nullptr;
        }
    }
}

void SBP2LoginSession::OnAgentResetComplete(uint16_t expectedGeneration,
                                            Async::AsyncStatus status,
                                            std::span<const uint8_t> response) noexcept {
    if (expectedGeneration != loginGeneration_) {
        return;
    }

    agentResetInProgress_ = false;

    // Clear ORB chain after reset
    ClearORBTracking(true);

    ASFW_LOG(SBP2, "SBP2LoginSession::OnAgentResetComplete: status=%s, ORB chain cleared",
             Async::ToString(status));

    if (agentResetCallback_) {
        int result = (status == Async::AsyncStatus::kSuccess) ? 0 : -1;
        auto cb = std::move(agentResetCallback_);
        agentResetCallback_ = nullptr;
        cb(result);
    }
}

// ---------------------------------------------------------------------------
// Unsolicited Status Enable
// ---------------------------------------------------------------------------

void SBP2LoginSession::EnableUnsolicitedStatus() noexcept {
    if (state_ != LoginState::LoggedIn) {
        unsolicitedStatusRequested_ = true;
        return;
    }

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    unsolicitedStatusWriteHandle_ = bus_.WriteQuad(
        gen, node, unsolicitedStatusAddress_, 0, speed,
        [this, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                     std::span<const uint8_t> response) {
            OnUnsolicitedStatusEnableComplete(requestGeneration, status, response);
        });
}

void SBP2LoginSession::OnUnsolicitedStatusEnableComplete(
    uint16_t expectedGeneration,
    Async::AsyncStatus status,
    std::span<const uint8_t> response) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::LoggedIn) {
        return;
    }

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession::OnUnsolicitedStatusEnableComplete: status=%s",
                 Async::ToString(status));
    }
}

} // namespace ASFW::Protocols::SBP2
