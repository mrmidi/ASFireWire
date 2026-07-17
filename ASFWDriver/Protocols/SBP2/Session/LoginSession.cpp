// LoginSession — SBP-2 login/reconnect/logout state machine. See LoginSession.hpp.
//
// Ported from PR #19's SBP2LoginSession, decomposed for DICE: the command plane
// moved to FetchAgent (composed here), timers go through ISessionScheduler (one
// cancelable management timer at a time) instead of the two-queue IOSleep model,
// and management async writes/the status-FIFO callback capture weak_from_this.

#include "LoginSession.hpp"

#include "../../../Async/Interfaces/IFireWireBus.hpp"
#include "../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../../Common/FWCommon.hpp"

#include <cstring>

namespace ASFW::Protocols::SBP2 {

using namespace ASFW::Protocols::SBP2::Wire;

// DICE's logging has no dedicated SBP-2 category; the session/command layer logs
// under the Async subsystem category (matching FetchAgent).
#define ASFW_LOG_SBP2(fmt, ...) ASFW_LOG(Async, fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

LoginSession::LoginSession(Async::IFireWireBus& bus,
                           Async::IFireWireBusInfo& busInfo,
                           AddressSpaceManager& addrSpaceMgr,
                           ISessionScheduler& scheduler)
    : bus_(bus)
    , busInfo_(busInfo)
    , addrSpaceMgr_(addrSpaceMgr)
    , scheduler_(scheduler)
    , fetchAgent_(bus, busInfo, scheduler) {}

LoginSession::~LoginSession() {
    CancelManagementTimer();
    if (loginWriteHandle_) {
        bus_.Cancel(loginWriteHandle_);
        loginWriteHandle_ = {};
    }
    if (reconnectWriteHandle_) {
        bus_.Cancel(reconnectWriteHandle_);
        reconnectWriteHandle_ = {};
    }
    if (logoutWriteHandle_) {
        bus_.Cancel(logoutWriteHandle_);
        logoutWriteHandle_ = {};
    }
    if (unsolicitedStatusWriteHandle_) {
        bus_.Cancel(unsolicitedStatusWriteHandle_);
        unsolicitedStatusWriteHandle_ = {};
    }
    if (busyTimeoutWriteHandle_) {
        bus_.Cancel(busyTimeoutWriteHandle_);
        busyTimeoutWriteHandle_ = {};
    }
    fetchAgent_.Unbind();
    DeallocateResources();
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void LoginSession::Configure(const SBP2TargetInfo& info) noexcept {
    targetInfo_ = info;
    configured_ = true;

    ASFW_LOG_SBP2(
             "LoginSession: configured target node=0x%04x mgmt_offset=%u LUN=%u "
             "mgmt_timeout=%ums max_orb=%u max_cmd_block=%u",
             info.targetNodeId, info.managementAgentOffset, info.lun,
             info.managementTimeoutMs, info.maxORBSize, info.maxCommandBlockSize);
}

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------

bool LoginSession::Login() noexcept {
    if (!configured_) {
        ASFW_LOG_SBP2( "LoginSession::Login: not configured");
        return false;
    }
    if (state_ == LoginState::LoggingIn || state_ == LoginState::LoggedIn) {
        ASFW_LOG_SBP2( "LoginSession::Login: state=%{public}s, ignoring", ToString(state_));
        return false;
    }

    if (!AllocateResources()) {
        ASFW_LOG_SBP2( "LoginSession::Login: resource allocation failed");
        SetState(LoginState::Failed);
        return false;
    }

    SetState(LoginState::LoggingIn);
    loginGeneration_ = static_cast<uint16_t>(busInfo_.GetGeneration().value);
    loginNodeID_ = targetInfo_.targetNodeId;

    BuildLoginORB();

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(TargetNodeShort())};
    const Async::FWAddress mgmtAddr{Async::FWAddress::QualifiedAddressParts{
        .addressHi = 0xFFFF,
        .addressLo = ManagementAgentAddressLo(targetInfo_.managementAgentOffset),
        .nodeID = loginNodeID_}};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    loginWriteHandle_ = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{loginORBAddressBE_.data(), loginORBAddressBE_.size()},
        speed,
        [weakSelf, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                         std::span<const uint8_t>) {
            if (auto self = weakSelf.lock()) {
                self->OnLoginWriteComplete(requestGeneration, status);
            }
        });

    if (!loginWriteHandle_) {
        ASFW_LOG_SBP2( "LoginSession::Login: WriteBlock failed immediately");
        SetState(LoginState::Failed);
        return false;
    }

    StartLoginTimer();
    return true;
}

// ---------------------------------------------------------------------------
// Logout
// ---------------------------------------------------------------------------

bool LoginSession::Logout() noexcept {
    if (state_ != LoginState::LoggedIn && state_ != LoginState::Suspended) {
        ASFW_LOG_SBP2( "LoginSession::Logout: state=%{public}s, ignoring", ToString(state_));
        return false;
    }

    SetState(LoginState::LoggingOut);
    fetchAgent_.Unbind();
    BuildLogoutORB();

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(TargetNodeShort())};
    const Async::FWAddress mgmtAddr{Async::FWAddress::QualifiedAddressParts{
        .addressHi = 0xFFFF,
        .addressLo = ManagementAgentAddressLo(targetInfo_.managementAgentOffset),
        .nodeID = loginNodeID_}};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    logoutWriteHandle_ = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{logoutORBAddressBE_.data(), logoutORBAddressBE_.size()},
        speed,
        [weakSelf, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                         std::span<const uint8_t>) {
            if (auto self = weakSelf.lock()) {
                self->OnLogoutWriteComplete(requestGeneration, status);
            }
        });

    if (!logoutWriteHandle_) {
        ASFW_LOG_SBP2( "LoginSession::Logout: WriteBlock failed");
        SetState(LoginState::Failed);
        return false;
    }

    StartLogoutTimer();
    return true;
}

// ---------------------------------------------------------------------------
// Reconnect
// ---------------------------------------------------------------------------

bool LoginSession::Reconnect() noexcept {
    if (state_ != LoginState::Suspended && state_ != LoginState::LoggedIn) {
        ASFW_LOG_SBP2( "LoginSession::Reconnect: state=%{public}s, ignoring", ToString(state_));
        return false;
    }

    SetState(LoginState::Reconnecting);
    loginGeneration_ = static_cast<uint16_t>(busInfo_.GetGeneration().value);
    loginNodeID_ = targetInfo_.targetNodeId;

    if (!AllocateResources()) {
        ASFW_LOG_SBP2( "LoginSession::Reconnect: resource allocation failed");
        SetState(LoginState::Failed);
        return false;
    }

    BuildReconnectORB();

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(TargetNodeShort())};
    const Async::FWAddress mgmtAddr{Async::FWAddress::QualifiedAddressParts{
        .addressHi = 0xFFFF,
        .addressLo = ManagementAgentAddressLo(targetInfo_.managementAgentOffset),
        .nodeID = loginNodeID_}};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    reconnectWriteHandle_ = bus_.WriteBlock(
        gen, node, mgmtAddr,
        std::span<const uint8_t>{reconnectORBAddressBE_.data(), reconnectORBAddressBE_.size()},
        speed,
        [weakSelf, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                         std::span<const uint8_t>) {
            if (auto self = weakSelf.lock()) {
                self->OnReconnectWriteComplete(requestGeneration, status);
            }
        });

    if (!reconnectWriteHandle_) {
        ASFW_LOG_SBP2( "LoginSession::Reconnect: WriteBlock failed, will retry");
        ArmManagementTimer(kLoginRetryDelayMs, [this]() { OnReconnectTimeout(); });
        return true;  // will retry
    }

    return true;
}

// ---------------------------------------------------------------------------
// Bus Reset Handling
// ---------------------------------------------------------------------------

void LoginSession::HandleBusReset(uint16_t newGeneration) noexcept {
    ASFW_LOG_SBP2( "LoginSession::HandleBusReset: state=%{public}s newGen=%u loginGen=%u",
             ToString(state_), newGeneration, loginGeneration_);

    switch (state_) {
        case LoginState::LoggingIn:
            CancelManagementTimer();
            loginRetryCount_ = 0;
            loginGeneration_ = newGeneration;
            fetchAgent_.Unbind();
            DeallocateResources();
            SetState(LoginState::Idle);
            ArmManagementTimer(100, [this]() { (void)Login(); });
            break;

        case LoginState::LoggedIn:
            CancelManagementTimer();
            fetchAgent_.Unbind();
            DeallocateResources();
            SetState(LoginState::Suspended);
            loginGeneration_ = newGeneration;
            break;

        case LoginState::Reconnecting:
            CancelManagementTimer();
            fetchAgent_.Unbind();
            DeallocateResources();
            loginGeneration_ = newGeneration;
            SetState(LoginState::Suspended);
            ArmManagementTimer(100, [this]() { (void)Reconnect(); });
            break;

        case LoginState::LoggingOut:
            CancelManagementTimer();
            fetchAgent_.Unbind();
            DeallocateResources();
            SetState(LoginState::Idle);
            break;

        case LoginState::Failed:
            DeallocateResources();
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

uint32_t LoginSession::ReconnectHoldSeconds() const noexcept {
    return reconnectHold_ > 0 ? (1u << reconnectHold_) : 0;
}

// ---------------------------------------------------------------------------
// Resource Allocation
// ---------------------------------------------------------------------------

bool LoginSession::AllocateResources() noexcept {
    const bool allResourcesAllocated = loginORBHandle_ != 0 && loginResponseHandle_ != 0 &&
                                       statusBlockHandle_ != 0 && reconnectORBHandle_ != 0 &&
                                       logoutORBHandle_ != 0;
    const bool anyResourceAllocated = loginORBHandle_ != 0 || loginResponseHandle_ != 0 ||
                                      statusBlockHandle_ != 0 || reconnectORBHandle_ != 0 ||
                                      logoutORBHandle_ != 0;

    if (allResourcesAllocated) {
        // Re-register the callback in case a reset path cleared it.
        RegisterStatusBlockCallback();
        return true;
    }

    if (anyResourceAllocated) {
        ASFW_LOG_SBP2( "LoginSession: inconsistent resource state before allocation");
        DeallocateResources();
    }

    if (!AllocateLoginORBAddressSpace()) {
        return false;
    }
    if (!AllocateLoginResponseAddressSpace()) {
        DeallocateResources();
        return false;
    }
    if (!AllocateStatusBlockAddressSpace()) {
        DeallocateResources();
        return false;
    }
    if (!AllocateReconnectORBAddressSpace()) {
        DeallocateResources();
        return false;
    }
    if (!AllocateLogoutORBAddressSpace()) {
        DeallocateResources();
        return false;
    }

    RegisterStatusBlockCallback();
    ASFW_LOG_SBP2( "LoginSession: all address spaces allocated");
    return true;
}

void LoginSession::RegisterStatusBlockCallback() noexcept {
    if (statusBlockHandle_ == 0) {
        return;
    }
    // AddressSpaceManager dispatches outside its lock, so never capture raw this.
    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    addrSpaceMgr_.SetRemoteWriteCallback(
        statusBlockHandle_,
        [weakSelf](uint64_t /*handle*/, uint32_t offset, std::span<const uint8_t> payload) {
            if (auto self = weakSelf.lock()) {
                self->OnStatusBlockRemoteWrite(offset, payload);
            }
        });
}

void LoginSession::DeallocateResources() noexcept {
    if (loginORBHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, loginORBHandle_);
        loginORBHandle_ = 0;
        loginORBMeta_ = {};
    }
    if (loginResponseHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, loginResponseHandle_);
        loginResponseHandle_ = 0;
        loginResponseMeta_ = {};
    }
    if (statusBlockHandle_) {
        addrSpaceMgr_.SetRemoteWriteCallback(statusBlockHandle_, {});
        addrSpaceMgr_.DeallocateAddressRange(this, statusBlockHandle_);
        statusBlockHandle_ = 0;
        statusBlockMeta_ = {};
    }
    if (reconnectORBHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, reconnectORBHandle_);
        reconnectORBHandle_ = 0;
        reconnectORBMeta_ = {};
    }
    if (logoutORBHandle_) {
        addrSpaceMgr_.DeallocateAddressRange(this, logoutORBHandle_);
        logoutORBHandle_ = 0;
        logoutORBMeta_ = {};
    }

    loginORBAddressBE_ = {};
    reconnectORBAddressBE_ = {};
    logoutORBAddressBE_ = {};
}

bool LoginSession::AllocateLoginORBAddressSpace() noexcept {
    const auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::LoginORB::kSize, &loginORBHandle_, &loginORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_SBP2( "LoginSession: failed to allocate login ORB address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(loginORBHandle_, "sbp2-login-orb");
    return true;
}

bool LoginSession::AllocateLoginResponseAddressSpace() noexcept {
    const auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::LoginResponse::kSize, &loginResponseHandle_, &loginResponseMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_SBP2( "LoginSession: failed to allocate login response address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(loginResponseHandle_, "sbp2-login-response");
    return true;
}

bool LoginSession::AllocateStatusBlockAddressSpace() noexcept {
    const auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::StatusBlock::kMaxSize, &statusBlockHandle_, &statusBlockMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_SBP2( "LoginSession: failed to allocate status block address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(statusBlockHandle_, "sbp2-status-fifo");
    return true;
}

bool LoginSession::AllocateReconnectORBAddressSpace() noexcept {
    const auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::ReconnectORB::kSize, &reconnectORBHandle_, &reconnectORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_SBP2( "LoginSession: failed to allocate reconnect ORB address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(reconnectORBHandle_, "sbp2-reconnect-orb");
    return true;
}

bool LoginSession::AllocateLogoutORBAddressSpace() noexcept {
    const auto kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        this, 0xFFFF, Wire::LogoutORB::kSize, &logoutORBHandle_, &logoutORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_SBP2( "LoginSession: failed to allocate logout ORB address space: 0x%08x", kr);
        return false;
    }
    addrSpaceMgr_.SetDebugLabel(logoutORBHandle_, "sbp2-logout-orb");
    return true;
}

// ---------------------------------------------------------------------------
// ORB Construction
// ---------------------------------------------------------------------------

void LoginSession::BuildLoginORB() noexcept {
    std::memset(&loginORBBuffer_, 0, sizeof(loginORBBuffer_));

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    loginORBBuffer_.loginResponseAddressHi =
        OSSwapHostToBigInt32(ComposeBusAddressHi(localNode, loginResponseMeta_.addressHi));
    loginORBBuffer_.loginResponseAddressLo = OSSwapHostToBigInt32(loginResponseMeta_.addressLo);
    loginORBBuffer_.options =
        static_cast<uint16_t>(Options::kLoginNotify | Options::kExclusiveLogin);
    loginORBBuffer_.lun = OSSwapHostToBigInt16(targetInfo_.lun);
    loginORBBuffer_.passwordLength = 0;
    loginORBBuffer_.loginResponseLength = OSSwapHostToBigInt16(sizeof(Wire::LoginResponse));
    loginORBBuffer_.statusFIFOAddressHi =
        OSSwapHostToBigInt32(ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    loginORBBuffer_.statusFIFOAddressLo = OSSwapHostToBigInt32(statusBlockMeta_.addressLo);

    addrSpaceMgr_.WriteLocalData(
        this, loginORBHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&loginORBBuffer_),
                                 sizeof(loginORBBuffer_)});

    // Management agent write payload: [nodeID(2)][addressHi(2)][addressLo(4)] BE.
    loginORBAddressBE_[0] = static_cast<uint8_t>(localNode >> 8);
    loginORBAddressBE_[1] = static_cast<uint8_t>(localNode & 0xFF);
    loginORBAddressBE_[2] = static_cast<uint8_t>(loginORBMeta_.addressHi >> 8);
    loginORBAddressBE_[3] = static_cast<uint8_t>(loginORBMeta_.addressHi & 0xFF);
    const uint32_t orbAddrLoBE = OSSwapHostToBigInt32(loginORBMeta_.addressLo);
    std::memcpy(&loginORBAddressBE_[4], &orbAddrLoBE, sizeof(uint32_t));
}

void LoginSession::BuildReconnectORB() noexcept {
    std::memset(&reconnectORBBuffer_, 0, sizeof(reconnectORBBuffer_));

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    reconnectORBBuffer_.options = Options::kReconnectNotify;
    reconnectORBBuffer_.loginID = OSSwapHostToBigInt16(loginID_);
    reconnectORBBuffer_.statusFIFOAddressHi =
        OSSwapHostToBigInt32(ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    reconnectORBBuffer_.statusFIFOAddressLo = OSSwapHostToBigInt32(statusBlockMeta_.addressLo);

    addrSpaceMgr_.WriteLocalData(
        this, reconnectORBHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&reconnectORBBuffer_),
                                 sizeof(reconnectORBBuffer_)});

    reconnectORBAddressBE_[0] = static_cast<uint8_t>(localNode >> 8);
    reconnectORBAddressBE_[1] = static_cast<uint8_t>(localNode & 0xFF);
    reconnectORBAddressBE_[2] = static_cast<uint8_t>(reconnectORBMeta_.addressHi >> 8);
    reconnectORBAddressBE_[3] = static_cast<uint8_t>(reconnectORBMeta_.addressHi & 0xFF);
    const uint32_t addrLoBE = OSSwapHostToBigInt32(reconnectORBMeta_.addressLo);
    std::memcpy(&reconnectORBAddressBE_[4], &addrLoBE, sizeof(uint32_t));
}

void LoginSession::BuildLogoutORB() noexcept {
    std::memset(&logoutORBBuffer_, 0, sizeof(logoutORBBuffer_));

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    logoutORBBuffer_.options = Options::kLogoutNotify;
    logoutORBBuffer_.loginID = OSSwapHostToBigInt16(loginID_);
    logoutORBBuffer_.statusFIFOAddressHi =
        OSSwapHostToBigInt32(ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    logoutORBBuffer_.statusFIFOAddressLo = OSSwapHostToBigInt32(statusBlockMeta_.addressLo);

    addrSpaceMgr_.WriteLocalData(
        this, logoutORBHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&logoutORBBuffer_),
                                 sizeof(logoutORBBuffer_)});

    logoutORBAddressBE_[0] = static_cast<uint8_t>(localNode >> 8);
    logoutORBAddressBE_[1] = static_cast<uint8_t>(localNode & 0xFF);
    logoutORBAddressBE_[2] = static_cast<uint8_t>(logoutORBMeta_.addressHi >> 8);
    logoutORBAddressBE_[3] = static_cast<uint8_t>(logoutORBMeta_.addressHi & 0xFF);
    const uint32_t addrLoBE = OSSwapHostToBigInt32(logoutORBMeta_.addressLo);
    std::memcpy(&logoutORBAddressBE_[4], &addrLoBE, sizeof(uint32_t));
}

// ---------------------------------------------------------------------------
// Command-plane binding
// ---------------------------------------------------------------------------

void LoginSession::BindFetchAgent() noexcept {
    const auto cbaAddr = [&](uint32_t offset) {
        return Async::FWAddress{Async::FWAddress::QualifiedAddressParts{
            .addressHi = commandBlockAgent_.addressHi,
            .addressLo = commandBlockAgent_.addressLo + offset,
            .nodeID = loginNodeID_}};
    };

    FetchAgent::Binding binding{};
    binding.generation = loginGeneration_;
    binding.nodeID = loginNodeID_;
    binding.fetchAgentAddress = cbaAddr(Wire::CommandBlockAgentOffsets::kFetchAgent);
    binding.agentResetAddress = cbaAddr(Wire::CommandBlockAgentOffsets::kAgentReset);
    binding.maxPayloadSize = maxPayloadSize_;
    fetchAgent_.Bind(binding);

    RefreshUnsolicitedStatusAddress();
}

void LoginSession::RefreshUnsolicitedStatusAddress() noexcept {
    unsolicitedStatusAddress_ = Async::FWAddress{Async::FWAddress::QualifiedAddressParts{
        .addressHi = commandBlockAgent_.addressHi,
        .addressLo = commandBlockAgent_.addressLo + Wire::CommandBlockAgentOffsets::kUnsolicitedStatusEnable,
        .nodeID = loginNodeID_}};
}

// ---------------------------------------------------------------------------
// Management Write Completions
// ---------------------------------------------------------------------------

void LoginSession::OnLoginWriteComplete(uint16_t expectedGeneration,
                                        Async::AsyncStatus status) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::LoggingIn) {
        return;
    }

    CancelManagementTimer();

    if (status != Async::AsyncStatus::kSuccess) {
        if (loginRetryCount_ < kLoginRetryMax) {
            loginRetryCount_++;
            ArmManagementTimer(kLoginRetryDelayMs, [this]() {
                loginGeneration_ = static_cast<uint16_t>(busInfo_.GetGeneration().value);
                loginNodeID_ = targetInfo_.targetNodeId;
                SetState(LoginState::Idle);
                (void)Login();
            });
            return;
        }

        ASFW_LOG_SBP2( "LoginSession: login retries exhausted");
        SetState(LoginState::Failed);
        if (loginCallback_) {
            LoginCompleteParams params{};
            params.status = -1;
            params.generation = loginGeneration_;
            loginCallback_(params);
        }
        return;
    }

    // Management agent write ACK'd. Wait for the device to write the status block;
    // restart the timer for the device processing window.
    StartLoginTimer();
}

void LoginSession::OnLoginTimeout() noexcept {
    if (state_ != LoginState::LoggingIn) {
        return;
    }

    if (loginRetryCount_ < kLoginRetryMax) {
        loginRetryCount_++;
        SetState(LoginState::Idle);
        (void)Login();
    } else {
        SetState(LoginState::Failed);
        if (loginCallback_) {
            LoginCompleteParams params{};
            params.status = -2;  // timeout
            params.generation = loginGeneration_;
            loginCallback_(params);
        }
    }
}

void LoginSession::OnReconnectWriteComplete(uint16_t expectedGeneration,
                                            Async::AsyncStatus status) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::Reconnecting) {
        return;
    }

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG_SBP2( "LoginSession::OnReconnectWriteComplete: status=%{public}s, retrying",
                 Async::ToString(status));
        ArmManagementTimer(100, [this]() { (void)Reconnect(); });
        return;
    }

    // Reconnect ORB write ACK'd. Wait for the status block from the device.
    StartReconnectTimer();
}

void LoginSession::OnReconnectTimeout() noexcept {
    if (state_ != LoginState::Reconnecting) {
        return;
    }
    ASFW_LOG_SBP2( "LoginSession: reconnect timeout, falling back to full login");
    SetState(LoginState::Idle);
    (void)Login();
}

void LoginSession::OnLogoutWriteComplete(uint16_t expectedGeneration,
                                         Async::AsyncStatus status) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::LoggingOut) {
        return;
    }

    if (status != Async::AsyncStatus::kSuccess) {
        CancelManagementTimer();
        loginID_ = 0;
        SetState(LoginState::Idle);
        if (logoutCallback_) {
            LogoutCompleteParams params{};
            params.status = -1;
            params.generation = loginGeneration_;
            logoutCallback_(params);
        }
        return;
    }

    StartLogoutTimer();
}

void LoginSession::OnLogoutTimeout() noexcept {
    if (state_ != LoginState::LoggingOut) {
        return;
    }
    ASFW_LOG_SBP2( "LoginSession: logout timeout, transitioning to Idle anyway");
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

void LoginSession::OnStatusBlockRemoteWrite(uint32_t offset,
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

    ASFW_LOG_SBP2( "LoginSession::OnStatusBlockRemoteWrite: state=%{public}s offset=%u len=%u sbpStatus=%u",
             ToString(state_), offset, len, block.sbpStatus);

    switch (state_) {
        case LoginState::LoggingIn:
            CancelManagementTimer();
            CompleteLoginFromStatusBlock(block, len);
            break;
        case LoginState::Reconnecting:
            CancelManagementTimer();
            CompleteReconnectFromStatusBlock(block, len);
            break;
        case LoginState::LoggingOut:
            CancelManagementTimer();
            CompleteLogoutFromStatusBlock(block, len);
            break;
        case LoginState::LoggedIn:
            ProcessStatusBlock(block, len);
            break;
        default:
            ASFW_LOG_SBP2( "LoginSession: unexpected status block in state %{public}s", ToString(state_));
            break;
    }
}

void LoginSession::CompleteLoginFromStatusBlock(const Wire::StatusBlock& block,
                                                uint32_t length) noexcept {
    if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        ASFW_LOG_SBP2( "LoginSession: login failed — sbpStatus=%u, retrying (%u/%u)",
                 block.sbpStatus, loginRetryCount_ + 1, kLoginRetryMax);
        if (loginRetryCount_ < kLoginRetryMax) {
            loginRetryCount_++;
            ArmManagementTimer(kLoginRetryDelayMs, [this]() {
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

    // Login succeeded — read the login response the device wrote to our space.
    std::vector<uint8_t> responseData;
    const auto kr = addrSpaceMgr_.ReadIncomingData(
        this, loginResponseHandle_, 0, sizeof(Wire::LoginResponse), &responseData);

    if (kr != kIOReturnSuccess || responseData.size() < sizeof(Wire::LoginResponse)) {
        ASFW_LOG_SBP2( "LoginSession: failed to read login response (kr=0x%08x, len=%zu)",
                 kr, responseData.size());
        if (loginRetryCount_ < kLoginRetryMax) {
            loginRetryCount_++;
            ArmManagementTimer(kLoginRetryDelayMs, [this]() {
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

    Wire::LoginResponse resp{};
    std::memcpy(&resp, responseData.data(), sizeof(resp));

    loginID_ = OSSwapBigToHostInt16(resp.loginID);
    reconnectHold_ = OSSwapBigToHostInt16(resp.reconnectHold);
    loginResponse_ = resp;

    const uint32_t cbaHi = OSSwapBigToHostInt32(resp.commandBlockAgentAddressHi);
    const uint32_t cbaLo = OSSwapBigToHostInt32(resp.commandBlockAgentAddressLo);
    commandBlockAgent_ = Async::FWAddress{Async::FWAddress::QualifiedAddressParts{
        .addressHi = static_cast<uint16_t>(cbaHi & 0xFFFFu),
        .addressLo = cbaLo,
        .nodeID = loginNodeID_}};

    loginRetryCount_ = 0;
    SetState(LoginState::LoggedIn);
    BindFetchAgent();

    // If unsolicited status was requested before login, enable it now (before the
    // busy-timeout write, matching PR #19's ordering).
    if (unsolicitedStatusRequested_) {
        unsolicitedStatusRequested_ = false;
        EnableUnsolicitedStatus();
    }

    ASFW_LOG_SBP2( "LoginSession: login successful — loginID=%u CBA=%04x:%08x reconnectHold=2^%u",
             loginID_, commandBlockAgent_.addressHi, commandBlockAgent_.addressLo, reconnectHold_);

    WriteBusyTimeout();

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

void LoginSession::CompleteReconnectFromStatusBlock(const Wire::StatusBlock& block,
                                                    uint32_t length) noexcept {
    if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        ASFW_LOG_SBP2( "LoginSession: reconnect failed — sbpStatus=%u, falling back to full login",
                 block.sbpStatus);
        SetState(LoginState::Idle);
        (void)Login();
        return;
    }

    SetState(LoginState::LoggedIn);
    BindFetchAgent();
    ASFW_LOG_SBP2( "LoginSession: reconnect successful — loginID=%u", loginID_);

    WriteBusyTimeout();

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

void LoginSession::CompleteLogoutFromStatusBlock(const Wire::StatusBlock& /*block*/,
                                                 uint32_t /*length*/) noexcept {
    const uint16_t oldLoginID = loginID_;
    loginID_ = 0;
    SetState(LoginState::Idle);
    ASFW_LOG_SBP2( "LoginSession: logout complete (was loginID=%u)", oldLoginID);

    if (logoutCallback_) {
        LogoutCompleteParams params{};
        params.status = 0;
        params.generation = loginGeneration_;
        logoutCallback_(params);
    }
}

void LoginSession::ProcessStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept {
    // Unsolicited: (details & 0xC0) == 0x80 (source bit set, resp == 0).
    const bool isUnsolicited = (block.details & 0xC0) == 0x80;

    if (statusCallback_) {
        statusCallback_(block, length);
    }

    if (isUnsolicited) {
        EnableUnsolicitedStatus();  // re-arm so the device can send more
        return;
    }

    (void)fetchAgent_.OnStatusBlock(block, length);
}

// ---------------------------------------------------------------------------
// Command plane (delegates to FetchAgent)
// ---------------------------------------------------------------------------

bool LoginSession::SubmitORB(SBP2CommandORB* orb) noexcept {
    if (state_ != LoginState::LoggedIn) {
        ASFW_LOG_SBP2( "LoginSession::SubmitORB: state=%{public}s, rejecting", ToString(state_));
        return false;
    }
    return fetchAgent_.Submit(orb);
}

void LoginSession::ResetFetchAgent(std::function<void(int)> callback) noexcept {
    fetchAgent_.Reset(std::move(callback));
}

// ---------------------------------------------------------------------------
// Unsolicited Status Enable
// ---------------------------------------------------------------------------

void LoginSession::EnableUnsolicitedStatus() noexcept {
    if (state_ != LoginState::LoggedIn) {
        unsolicitedStatusRequested_ = true;
        return;
    }

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(TargetNodeShort())};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    unsolicitedStatusWriteHandle_ = bus_.WriteQuad(
        gen, node, unsolicitedStatusAddress_, 0, speed,
        [weakSelf, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                         std::span<const uint8_t>) {
            if (auto self = weakSelf.lock()) {
                self->OnUnsolicitedStatusEnableComplete(requestGeneration, status);
            }
        });
}

void LoginSession::OnUnsolicitedStatusEnableComplete(uint16_t expectedGeneration,
                                                     Async::AsyncStatus status) noexcept {
    if (expectedGeneration != loginGeneration_ || state_ != LoginState::LoggedIn) {
        return;
    }
    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG_SBP2( "LoginSession::OnUnsolicitedStatusEnableComplete: status=%{public}s",
                 Async::ToString(status));
    }
}

// ---------------------------------------------------------------------------
// Busy Timeout
// ---------------------------------------------------------------------------

void LoginSession::WriteBusyTimeout() noexcept {
    if (busyTimeoutInProgress_) {
        bus_.Cancel(busyTimeoutWriteHandle_);
        busyTimeoutInProgress_ = false;
    }

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(TargetNodeShort())};
    const Async::FWAddress busyAddr{Async::FWAddress::QualifiedAddressParts{
        .addressHi = kCSRBusAddressHi,
        .addressLo = kBusyTimeoutAddressLo,
        .nodeID = loginNodeID_}};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    busyTimeoutInProgress_ = true;
    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    busyTimeoutWriteHandle_ = bus_.WriteBlock(
        gen, node, busyAddr,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&busyTimeoutBuffer_), 4},
        speed,
        [weakSelf, requestGeneration = loginGeneration_](Async::AsyncStatus status,
                                                         std::span<const uint8_t>) {
            if (auto self = weakSelf.lock()) {
                self->OnBusyTimeoutComplete(requestGeneration, status);
            }
        });

    if (!busyTimeoutWriteHandle_) {
        ASFW_LOG_SBP2( "LoginSession::WriteBusyTimeout: WriteBlock failed");
        busyTimeoutInProgress_ = false;
    }
}

void LoginSession::OnBusyTimeoutComplete(uint16_t expectedGeneration,
                                         Async::AsyncStatus status) noexcept {
    if (expectedGeneration != loginGeneration_) {
        return;
    }
    busyTimeoutInProgress_ = false;
    if (status != Async::AsyncStatus::kSuccess && status != Async::AsyncStatus::kAborted) {
        ASFW_LOG_SBP2( "LoginSession::OnBusyTimeoutComplete: status=%{public}s", Async::ToString(status));
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void LoginSession::SetState(LoginState newState) noexcept {
    if (state_ != newState) {
        ASFW_LOG_SBP2( "LoginSession: state %{public}s -> %{public}s", ToString(state_), ToString(newState));
        state_ = newState;
    }
}

void LoginSession::StartLoginTimer() noexcept {
    ArmManagementTimer(targetInfo_.managementTimeoutMs, [this]() { OnLoginTimeout(); });
}

void LoginSession::StartReconnectTimer() noexcept {
    ArmManagementTimer(targetInfo_.managementTimeoutMs + 1000, [this]() { OnReconnectTimeout(); });
}

void LoginSession::StartLogoutTimer() noexcept {
    ArmManagementTimer(targetInfo_.managementTimeoutMs, [this]() { OnLogoutTimeout(); });
}

void LoginSession::ArmManagementTimer(uint64_t delayMs, std::function<void()> fn) noexcept {
    CancelManagementTimer();
    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    managementTimerToken_ = scheduler_.ScheduleAfter(
        delayMs * 1'000'000ULL, [weakSelf, fn = std::move(fn)]() mutable {
            if (auto self = weakSelf.lock()) {
                self->managementTimerToken_ = kInvalidSchedulerToken;
                fn();
            }
        });
}

void LoginSession::CancelManagementTimer() noexcept {
    if (managementTimerToken_ != kInvalidSchedulerToken) {
        scheduler_.Cancel(managementTimerToken_);
        managementTimerToken_ = kInvalidSchedulerToken;
    }
}

} // namespace ASFW::Protocols::SBP2
