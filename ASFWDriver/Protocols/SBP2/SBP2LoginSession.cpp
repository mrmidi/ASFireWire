#include "SBP2LoginSession.hpp"
#include "AddressSpaceManager.hpp"

#include "../../Async/Interfaces/IFireWireBus.hpp"
#include "../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../Common/FWCommon.hpp"

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
    DeallocateResources();
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
        [this](Async::AsyncStatus status, std::span<const uint8_t> response) {
            OnLoginWriteComplete(status, response);
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
        [this](Async::AsyncStatus status, std::span<const uint8_t> response) {
            OnLogoutWriteComplete(status, response);
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
        [this](Async::AsyncStatus status, std::span<const uint8_t> response) {
            OnReconnectWriteComplete(status, response);
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
            SubmitDelayedCallback(100, [this]() {
                Login();
            });
            break;

        case LoginState::LoggedIn:
            // Transition to Suspended — wait for topology then reconnect.
            SetState(LoginState::Suspended);
            loginGeneration_ = newGeneration;
            break;

        case LoginState::Reconnecting:
            // Reconnect was in flight — retry.
            reconnectTimerActive_ = false;
            SubmitDelayedCallback(100, [this]() {
                Reconnect();
            });
            break;

        case LoginState::LoggingOut:
            // Logout in flight during bus reset — consider logged out.
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
    lastORB_ = nullptr;
    deferredORB_ = nullptr;

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
    // Use address Hi=0xFFFF (initial CSR space), Lo=auto.
    auto kr = addrSpaceMgr_.AllocateAddressRange(
        this, 0xFFFF, 0, Wire::LoginORB::kSize,
        &loginORBHandle_, &loginORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate login ORB address space: 0x%08x", kr);
        return false;
    }
    return true;
}

bool SBP2LoginSession::AllocateLoginResponseAddressSpace() noexcept {
    // Login response is 16 bytes, writable by target device.
    auto kr = addrSpaceMgr_.AllocateAddressRange(
        this, 0xFFFF, 0, Wire::LoginResponse::kSize,
        &loginResponseHandle_, &loginResponseMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate login response address space: 0x%08x", kr);
        return false;
    }
    return true;
}

bool SBP2LoginSession::AllocateStatusBlockAddressSpace() noexcept {
    // Status block is up to 32 bytes, writable by target device.
    auto kr = addrSpaceMgr_.AllocateAddressRange(
        this, 0xFFFF, 0, Wire::StatusBlock::kMaxSize,
        &statusBlockHandle_, &statusBlockMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate status block address space: 0x%08x", kr);
        return false;
    }
    return true;
}

bool SBP2LoginSession::AllocateReconnectORBAddressSpace() noexcept {
    auto kr = addrSpaceMgr_.AllocateAddressRange(
        this, 0xFFFF, 0, Wire::ReconnectORB::kSize,
        &reconnectORBHandle_, &reconnectORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate reconnect ORB address space: 0x%08x", kr);
        return false;
    }
    return true;
}

bool SBP2LoginSession::AllocateLogoutORBAddressSpace() noexcept {
    auto kr = addrSpaceMgr_.AllocateAddressRange(
        this, 0xFFFF, 0, Wire::LogoutORB::kSize,
        &logoutORBHandle_, &logoutORBMeta_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession: failed to allocate logout ORB address space: 0x%08x", kr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ORB Construction
// ---------------------------------------------------------------------------

void SBP2LoginSession::BuildLoginORB() noexcept {
    std::memset(&loginORBBuffer_, 0, sizeof(loginORBBuffer_));

    // Get local node ID for filling address fields.
    const uint16_t localNode = static_cast<uint16_t>(busInfo_.GetLocalNodeID().value);

    // Login response address: nodeID in upper 16 bits of addressHi.
    const uint32_t responseAddrHi = ToBE32(
        (static_cast<uint32_t>(loginResponseMeta_.addressHi)) |
        (static_cast<uint32_t>(localNode) << 16));
    const uint32_t responseAddrLo = ToBE32(loginResponseMeta_.addressLo);

    // Status FIFO address.
    const uint32_t statusAddrHi = ToBE32(
        (static_cast<uint32_t>(statusBlockMeta_.addressHi)) |
        (static_cast<uint32_t>(localNode) << 16));
    const uint32_t statusAddrLo = ToBE32(statusBlockMeta_.addressLo);

    // Fill login ORB fields.
    loginORBBuffer_.loginResponseAddressHi = responseAddrHi;
    loginORBBuffer_.loginResponseAddressLo = responseAddrLo;
    loginORBBuffer_.options = Options::kExclusiveLogin;
    loginORBBuffer_.loginResponseLength = ToBE16(sizeof(Wire::LoginResponse));
    loginORBBuffer_.lun = ToBE16(targetInfo_.lun);
    loginORBBuffer_.passwordLength = 0;
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
             "SBP2LoginSession::BuildLoginORB: ORB at %04x:%08x, response at %04x:%08x, "
             "status at %04x:%08x, LUN=%u",
             localNode, loginORBMeta_.addressLo,
             localNode, loginResponseMeta_.addressLo,
             localNode, statusBlockMeta_.addressLo,
             targetInfo_.lun);
}

void SBP2LoginSession::BuildReconnectORB() noexcept {
    std::memset(&reconnectORBBuffer_, 0, sizeof(reconnectORBBuffer_));

    const uint16_t localNode = static_cast<uint16_t>(busInfo_.GetLocalNodeID().value);

    // Reconnect ORB: options = reconnect (3) | notify
    reconnectORBBuffer_.options = Options::kReconnectNotify;
    reconnectORBBuffer_.loginID = ToBE16(loginID_);

    // Status FIFO address — reuse the dedicated status block address space.
    const uint32_t statusAddrHi = ToBE32(
        (static_cast<uint32_t>(statusBlockMeta_.addressHi)) |
        (static_cast<uint32_t>(localNode) << 16));
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

    const uint16_t localNode = static_cast<uint16_t>(busInfo_.GetLocalNodeID().value);

    logoutORBBuffer_.options = Options::kLogoutNotify;
    logoutORBBuffer_.loginID = ToBE16(loginID_);

    // Status FIFO address — reuse the dedicated status block address space.
    const uint32_t statusAddrHi = ToBE32(
        (static_cast<uint32_t>(statusBlockMeta_.addressHi)) |
        (static_cast<uint32_t>(localNode) << 16));
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

void SBP2LoginSession::OnLoginWriteComplete(Async::AsyncStatus status,
                                            std::span<const uint8_t> response) noexcept {
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
                Login();
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

    ASFW_LOG(SBP2, "SBP2LoginSession: login timeout (%u/%u)", loginRetryCount_ + 1, kLoginRetryMax);

    if (loginRetryCount_ < kLoginRetryMax) {
        loginRetryCount_++;
        SetState(LoginState::Idle);
        Login();
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

void SBP2LoginSession::OnReconnectWriteComplete(Async::AsyncStatus status,
                                                 std::span<const uint8_t> response) noexcept {
    reconnectTimerActive_ = false;

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession::OnReconnectWriteComplete: status=%s, retrying",
                 Async::ToString(status));

        SubmitDelayedCallback(100, [this]() { Reconnect(); });
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
    Login();
}

void SBP2LoginSession::OnLogoutWriteComplete(Async::AsyncStatus status,
                                              std::span<const uint8_t> response) noexcept {
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
                Login();
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
                Login();
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
        Login();
        return;
    }

    SetState(LoginState::LoggedIn);
    ASFW_LOG(SBP2, "SBP2LoginSession: reconnect successful — loginID=%u", loginID_);

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
    // In LoggedIn state, status blocks signal ORB completion.
    // The status block contains orbOffsetHi/orbOffsetLo that identifies which ORB
    // completed. Currently we only support single-ORB-in-flight and match via
    // lastORB_. Multi-ORB matching will require an ORB list keyed by offset.
    if (statusCallback_) {
        statusCallback_(block, length);
    }

    if (lastORB_ != nullptr) {
        lastORB_->CancelTimer();
        auto& cb = lastORB_->GetCompletionCallback();
        if (cb) {
            int status = 0;
            if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo &&
                block.sbpStatus != Wire::SBPStatus::kDummyORBCompleted) {
                status = -static_cast<int>(block.sbpStatus);
            }
            cb(status);
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

void SBP2LoginSession::SetWorkQueue(IODispatchQueue* queue) noexcept {
    workQueue_ = queue;
}

void SBP2LoginSession::CancelPendingTimer() noexcept {
    // With the DispatchAsync + IOSleep pattern we can't truly cancel in-flight
    // sleeps, but timeout handlers check state_ before acting.
    pendingTimerCallback_ = nullptr;
}

void SBP2LoginSession::SubmitDelayedCallback(uint64_t delayMs,
                                              std::function<void()> callback) noexcept {
    pendingTimerCallback_ = callback;

    if (workQueue_) {
        // Capture by value to avoid use-after-free if callback is replaced.
        auto cb = std::move(callback);
        workQueue_->DispatchAsync(^{
#ifdef ASFW_HOST_TEST
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
#else
            IOSleep(static_cast<uint32_t>(delayMs));
#endif
            cb();
        });
    }
}

// ---------------------------------------------------------------------------
// ORB Submission
// ---------------------------------------------------------------------------

bool SBP2LoginSession::SubmitORB(SBP2CommandORB* orb) noexcept {
    if (state_ != LoginState::LoggedIn) {
        ASFW_LOG(SBP2, "SBP2LoginSession::SubmitORB: state=%s, rejecting", ToString(state_));
        return false;
    }

    if (orb == nullptr || orb->IsAppended()) {
        ASFW_LOG(SBP2, "SBP2LoginSession::SubmitORB: invalid ORB (null=%d, appended=%d)",
                 orb == nullptr, orb != nullptr && orb->IsAppended());
        return false;
    }

    // Compute fetch agent and doorbell addresses from CBA.
    fetchAgentAddress_ = Async::FWAddress{
        Async::FWAddress::QualifiedAddressParts{
            .addressHi = commandBlockAgent_.addressHi,
            .addressLo = commandBlockAgent_.addressLo + Wire::CommandBlockAgentOffsets::kORBPointer,
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

    const uint16_t localNode = static_cast<uint16_t>(busInfo_.GetLocalNodeID().value);
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

    orb->PrepareForExecution(localNode, speed, maxPayloadLog);
    orb->SetFetchAgentWriteRetries(20);

    const bool isImmediate = (orb->GetFlags() & SBP2CommandORB::kImmediate) != 0;

    if (isImmediate) {
        // Immediate: write ORB address directly to fetch agent
        lastORB_ = orb;

        if (fetchAgentWriteInUse_) {
            // Fetch agent write in flight — defer until completion
            deferredORB_ = orb;
            ASFW_LOG(SBP2, "SBP2LoginSession::SubmitORB: fetch agent busy, deferring ORB");
            return true;
        }

        AppendORBImmediate(orb);
    } else {
        // Chained: link to last ORB, ring doorbell
        AppendORB(orb);
        RingDoorbell();
    }

    return true;
}

void SBP2LoginSession::AppendORBImmediate(SBP2CommandORB* orb) noexcept {
    // Cancel any in-flight fetch agent write
    if (fetchAgentWriteInUse_ && fetchAgentWriteHandle_) {
        bus_.Cancel(fetchAgentWriteHandle_);
    }

    // Build 8-byte ORB address in big-endian
    // Format: [nodeID(2)][addressHi(2)][addressLo(4)]
    const uint16_t localNode = static_cast<uint16_t>(busInfo_.GetLocalNodeID().value);
    const Async::FWAddress orbAddr = orb->GetORBAddress();

    fetchAgentWriteData_[0] = static_cast<uint8_t>(localNode >> 8);
    fetchAgentWriteData_[1] = static_cast<uint8_t>(localNode & 0xFF);
    fetchAgentWriteData_[2] = static_cast<uint8_t>(orbAddr.addressHi >> 8);
    fetchAgentWriteData_[3] = static_cast<uint8_t>(orbAddr.addressHi & 0xFF);
    const uint32_t addrLoBE = ToBE32(orbAddr.addressLo);
    std::memcpy(&fetchAgentWriteData_[4], &addrLoBE, sizeof(uint32_t));

    fetchAgentWriteInUse_ = true;

    const FW::Generation gen{loginGeneration_};
    const FW::NodeId node{static_cast<uint8_t>(loginNodeID_ & 0x3Fu)};
    const FW::FwSpeed speed = busInfo_.GetSpeed(node);

    fetchAgentWriteHandle_ = bus_.WriteBlock(
        gen, node, fetchAgentAddress_,
        std::span<const uint8_t>{fetchAgentWriteData_.data(), fetchAgentWriteData_.size()},
        speed,
        [this](Async::AsyncStatus status, std::span<const uint8_t> response) {
            OnFetchAgentWriteComplete(status, response);
        });

    if (!fetchAgentWriteHandle_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::AppendORBImmediate: WriteBlock failed");
        fetchAgentWriteInUse_ = false;
    }

    ASFW_LOG(SBP2,
             "SBP2LoginSession::AppendORBImmediate: wrote ORB addr %04x:%08x to fetch agent",
             localNode, orbAddr.addressLo);
}

void SBP2LoginSession::AppendORB(SBP2CommandORB* orb) noexcept {
    if (lastORB_ == nullptr) {
        // First ORB — write directly to fetch agent instead of chaining
        lastORB_ = orb;
        AppendORBImmediate(orb);
        return;
    }

    if (lastORB_ != orb) {
        const Async::FWAddress orbAddr = orb->GetORBAddress();

        // Set the new ORB's address in big-endian into the last ORB's next pointer
        const uint16_t localNode = static_cast<uint16_t>(busInfo_.GetLocalNodeID().value);
        const uint32_t nextHi = ToBE32(
            (static_cast<uint32_t>(localNode) << 16) | orbAddr.addressHi);
        const uint32_t nextLo = ToBE32(orbAddr.addressLo);
        lastORB_->SetNextORBAddress(nextHi, nextLo);

        lastORB_ = orb;
    }
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
        [this](Async::AsyncStatus status, std::span<const uint8_t> response) {
            OnDoorbellComplete(status, response);
        });

    if (!doorbellWriteHandle_) {
        ASFW_LOG(SBP2, "SBP2LoginSession::RingDoorbell: WriteQuad failed");
        doorbellInProgress_ = false;
    }
}

void SBP2LoginSession::OnFetchAgentWriteComplete(Async::AsyncStatus status,
                                                   std::span<const uint8_t> response) noexcept {
    fetchAgentWriteInUse_ = false;

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2,
                 "SBP2LoginSession::OnFetchAgentWriteComplete: status=%s, retries=%u",
                 Async::ToString(status),
                 lastORB_ ? lastORB_->GetFetchAgentWriteRetries() : 0);

        if (lastORB_ != nullptr) {
            uint32_t retries = lastORB_->GetFetchAgentWriteRetries();
            if (retries > 0) {
                retries--;
                lastORB_->SetFetchAgentWriteRetries(retries);
                // Retry after a delay
                SubmitDelayedCallback(1000, [this]() {
                    AppendORBImmediate(lastORB_);
                });
                return;
            }

            // Retries exhausted — report failure
            auto& cb = lastORB_->GetCompletionCallback();
            if (cb) {
                cb(-1);
            }
        }
        return;
    }

    // Fetch agent write succeeded. Submit deferred ORB if any.
    SBP2CommandORB* deferred = deferredORB_;
    deferredORB_ = nullptr;

    if (deferred != nullptr) {
        ASFW_LOG(SBP2, "SBP2LoginSession: submitting deferred ORB");
        AppendORBImmediate(deferred);
    }
}

void SBP2LoginSession::OnDoorbellComplete(Async::AsyncStatus status,
                                            std::span<const uint8_t> response) noexcept {
    doorbellInProgress_ = false;

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(SBP2, "SBP2LoginSession::OnDoorbellComplete: status=%s",
                 Async::ToString(status));
    }

    if (doorbellRingAgain_) {
        doorbellRingAgain_ = false;
        RingDoorbell();
    }
}

} // namespace ASFW::Protocols::SBP2
