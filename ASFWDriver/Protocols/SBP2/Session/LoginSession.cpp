// LoginSession — SBP-2 login/reconnect/logout. See LoginSession.hpp; the
// transitions are LoginFsm (LoginStateMachine.hpp).
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
namespace Fsm = LoginFsm;

// DICE's logging has no dedicated SBP-2 category; the session/command layer logs
// under the Async subsystem category (matching FetchAgent).
#define ASFW_LOG_SBP2(fmt, ...) ASFW_LOG(Async, fmt, ##__VA_ARGS__)

namespace {

uint64_t PackSnapshot(const Fsm::State& state) noexcept {
    const auto* held = Fsm::Held(state);
    return static_cast<uint64_t>(Fsm::Reported(state)) |
           (static_cast<uint64_t>(Fsm::GenerationOf(state)) << 8) |
           (static_cast<uint64_t>(held ? held->id : 0) << 24) |
           (static_cast<uint64_t>(held != nullptr) << 40);
}

} // namespace

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
    , fetchAgent_(bus, busInfo, scheduler)
    , executorLock_(IOLockAlloc()) {}

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
    if (executorLock_) {
        IOLockFree(executorLock_);
    }
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
// Session operations
// ---------------------------------------------------------------------------

bool LoginSession::Login() noexcept {
    if (!configured_) {
        ASFW_LOG_SBP2( "LoginSession::Login: not configured");
        return false;
    }
    return Request(Fsm::Ev::Login{}, "Login");
}

bool LoginSession::Logout() noexcept {
    return Request(Fsm::Ev::Logout{}, "Logout");
}

bool LoginSession::Resume() noexcept {
    return Request(Fsm::Ev::Resume{}, "Resume");
}

void LoginSession::HandleBusReset(uint16_t newGeneration) noexcept {
    Serialized([this, newGeneration] {
        ASFW_LOG_SBP2( "LoginSession::HandleBusReset: state=%{public}s newGen=%u gen=%u",
                 Fsm::Name(fsm_), newGeneration, Fsm::GenerationOf(fsm_));
        (void)Apply(Fsm::Ev::BusReset{newGeneration});
    });
}

void LoginSession::AbandonSuspended() noexcept {
    Serialized([this] {
        if (Fsm::Reported(fsm_) != LoginState::Suspended) {
            return;
        }
        ASFW_LOG_SBP2("LoginSession: device gone while suspended — session lost");
        (void)Apply(Fsm::Ev::DeviceGone{});
    });
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void LoginSession::Serialized(std::function<void()> work) noexcept {
    IOLockLock(executorLock_);
    pendingWork_.push_back(std::move(work));
    if (executing_) {
        IOLockUnlock(executorLock_);
        return;
    }
    executing_ = true;
    IOLockUnlock(executorLock_);

    // A callback may drop the registry's reference mid-transition.
    const auto keepAlive = weak_from_this().lock();
    for (;;) {
        IOLockLock(executorLock_);
        if (pendingWork_.empty()) {
            executing_ = false;
            IOLockUnlock(executorLock_);
            return;
        }
        auto next = std::move(pendingWork_.front());
        pendingWork_.pop_front();
        IOLockUnlock(executorLock_);
        next();
    }
}

void LoginSession::Dispatch(Fsm::Event event) noexcept {
    Serialized([this, event = std::move(event)] { (void)Apply(event); });
}

bool LoginSession::Request(Fsm::Event event, const char* what) noexcept {
    // Shared: if another thread is running work, ours runs after we return.
    const auto accepted = std::make_shared<std::optional<bool>>();
    Serialized([this, accepted, what, event = std::move(event)] {
        *accepted = Apply(event);
        if (!**accepted) {
            ASFW_LOG_SBP2( "LoginSession::%{public}s: state=%{public}s, ignoring", what,
                     Fsm::Name(fsm_));
        }
    });
    if (!accepted->has_value()) {
        return true;
    }
    // A write refused on the spot is queued behind the request and has run by
    // now; it leaves the session Failed.
    return **accepted && Snapshot().state != LoginState::Failed;
}

bool LoginSession::Apply(const Fsm::Event& event) noexcept {
    auto step = Fsm::Transition(fsm_, event, Now());
    if (!step) {
        return false;
    }
    const Fsm::Effects& fx = step->fx;

    if (fx.cancelTimer) {
        CancelManagementTimer();
    }
    if (fx.unbind) {
        fetchAgent_.Unbind();
    }
    if (fx.release) {
        DeallocateResources();
    }
    // Down-edge for the old login before the fallback login's state is visible.
    if (fx.sessionLostFirst) {
        NotifySessionLost(fx.generation);
    }
    Commit(std::move(step->next));
    if (fx.write) {
        IssueWrite(*fx.write);
    }
    if (fx.arm) {
        ArmTimer(*fx.arm);
    }
    if (fx.bind) {
        BindCommandPlane(*fx.bind);
    }
    Notify(fx);
    return true;
}

void LoginSession::Commit(Fsm::State next) noexcept {
    const char* from = Fsm::Name(fsm_);
    const char* to = Fsm::Name(next);
    if (std::strcmp(from, to) != 0) {
        ASFW_LOG_SBP2( "LoginSession: state %{public}s -> %{public}s", from, to);
    }
    fsm_ = std::move(next);
    snapshot_.store(PackSnapshot(fsm_), std::memory_order_release);
}

Fsm::Route LoginSession::Now() const noexcept {
    return Fsm::Route{static_cast<uint16_t>(busInfo_.GetGeneration().value),
                      targetInfo_.targetNodeId};
}

LoginSnapshot LoginSession::Snapshot() const noexcept {
    const uint64_t word = snapshot_.load(std::memory_order_acquire);
    LoginSnapshot snap{};
    snap.state = static_cast<LoginState>(word & 0xFFu);
    snap.generation = static_cast<uint16_t>(word >> 8);
    if ((word >> 40) & 1u) {
        snap.loginID = static_cast<uint16_t>(word >> 24);
    }
    return snap;
}

void LoginSession::IssueWrite(Fsm::Op op) noexcept {
    const auto route = Fsm::RouteOf(fsm_);
    if (!route) {
        return;
    }
    // Every management ORB needs the address spaces; a reset released them.
    if (!AllocateResources()) {
        ASFW_LOG_SBP2( "LoginSession: resource allocation failed");
        Dispatch(Fsm::Ev::ResourcesUnavailable{});
        return;
    }

    const auto* held = Fsm::Held(fsm_);
    Async::AsyncHandle* handle = nullptr;
    switch (op) {
        case Fsm::Op::Login:
            BuildLoginORB();
            handle = &loginWriteHandle_;
            break;
        case Fsm::Op::Reconnect:
            BuildReconnectORB(held ? held->id : 0);
            handle = &reconnectWriteHandle_;
            break;
        case Fsm::Op::Logout:
            BuildLogoutORB(held ? held->id : 0);
            handle = &logoutWriteHandle_;
            break;
    }

    const auto& orbAddress = OrbAddressFor(op);
    const FW::NodeId node = ShortNode(route->node);
    const Async::FWAddress mgmtAddr{Async::FWAddress::QualifiedAddressParts{
        .addressHi = 0xFFFF,
        .addressLo = ManagementAgentAddressLo(targetInfo_.managementAgentOffset),
        .nodeID = route->node}};

    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    *handle = bus_.WriteBlock(
        FW::Generation{route->generation}, node, mgmtAddr,
        std::span<const uint8_t>{orbAddress.data(), orbAddress.size()},
        busInfo_.GetSpeed(node),
        [weakSelf, op, generation = route->generation](Async::AsyncStatus status,
                                                       std::span<const uint8_t>) {
            if (auto self = weakSelf.lock()) {
                if (status != Async::AsyncStatus::kSuccess) {
                    ASFW_LOG_SBP2( "LoginSession: management write status=%{public}s",
                             Async::ToString(status));
                }
                self->Dispatch(Fsm::Ev::WriteDone{
                    op, generation, status == Async::AsyncStatus::kSuccess});
            }
        });

    if (!*handle) {
        ASFW_LOG_SBP2( "LoginSession: management WriteBlock failed immediately");
        Dispatch(Fsm::Ev::WriteRefused{op});
    }
}

void LoginSession::ArmTimer(Fsm::Timer timer) noexcept {
    const uint64_t mgmtMs = targetInfo_.managementTimeoutMs;
    switch (timer) {
        case Fsm::Timer::LoginTimeout:
            ArmManagementTimer(mgmtMs, Fsm::Ev::Timeout{Fsm::Op::Login});
            break;
        case Fsm::Timer::LoginRetry:
            ArmManagementTimer(kRetryDelayMs, Fsm::Ev::LoginRetryDue{});
            break;
        case Fsm::Timer::ReconnectTimeout:
            ArmManagementTimer(mgmtMs + 1000, Fsm::Ev::Timeout{Fsm::Op::Reconnect});
            break;
        case Fsm::Timer::ReconnectRetry:
            ArmManagementTimer(kRetryDelayMs, Fsm::Ev::Timeout{Fsm::Op::Reconnect});
            break;
        case Fsm::Timer::LogoutTimeout:
            ArmManagementTimer(mgmtMs, Fsm::Ev::Timeout{Fsm::Op::Logout});
            break;
    }
}

void LoginSession::Notify(const Fsm::Effects& fx) noexcept {
    switch (fx.notify) {
        case Fsm::Notify::None:
            return;
        case Fsm::Notify::SessionLost:
            NotifySessionLost(fx.generation);
            return;
        case Fsm::Notify::LoggedIn:
        case Fsm::Notify::LoginFailed:
            if (loginCallback_) {
                LoginCompleteParams params{};
                params.status = fx.status;
                if (const auto* held = Fsm::Held(fsm_)) {
                    params.loginResponse = held->response;
                }
                params.generation = fx.generation;
                loginCallback_(params);
            }
            return;
        case Fsm::Notify::LogoutDone:
            if (logoutCallback_) {
                LogoutCompleteParams params{};
                params.status = fx.status;
                params.generation = fx.generation;
                logoutCallback_(params);
            }
            return;
    }
}

// A rejected or unanswered reconnect means the target dropped the old login —
// typically a device power-cycle, which also reverts its command state
// (initialization such as MODE SELECT is gone). Emit the terminal down-edge
// for the old session BEFORE the fallback login: observers tear down state
// keyed to it (the HBA destroys SCSI target 0), and the fresh login's up-edge
// rebuilds it, forcing a full SAM re-probe/re-init against the rebooted device
// instead of reusing its stale identity. Matches Apple's model — IOFireWireSBP2
// reports a failed reconnect as a lost session to its clients rather than
// silently re-logging-in underneath them.
void LoginSession::NotifySessionLost(uint16_t generation) noexcept {
    if (!loginCallback_) {
        return;
    }
    LoginCompleteParams params{};
    params.status = Fsm::kStatusSessionLost;
    params.generation = generation;
    loginCallback_(params);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

uint32_t LoginSession::ReconnectHoldSeconds() const noexcept {
    const auto* held = Fsm::Held(fsm_);
    return held && held->reconnectHold > 0 ? (1u << held->reconnectHold) : 0;
}

Async::FWAddress LoginSession::CommandBlockAgent() const noexcept {
    const auto* held = Fsm::Held(fsm_);
    if (!held) {
        return {};
    }
    const auto route = Fsm::RouteOf(fsm_);
    return Async::FWAddress{Async::FWAddress::QualifiedAddressParts{
        .addressHi = held->commandBlockAgentHi,
        .addressLo = held->commandBlockAgentLo,
        .nodeID = route ? route->node : targetInfo_.targetNodeId}};
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

namespace {

// Management agent write payload: [nodeID(2)][addressHi(2)][addressLo(4)] BE.
void EncodeOrbAddress(std::array<uint8_t, 8>& out, uint16_t localNode,
                      const AddressSpaceManager::AddressRangeMeta& meta) noexcept {
    out[0] = static_cast<uint8_t>(localNode >> 8);
    out[1] = static_cast<uint8_t>(localNode & 0xFF);
    out[2] = static_cast<uint8_t>(meta.addressHi >> 8);
    out[3] = static_cast<uint8_t>(meta.addressHi & 0xFF);
    const uint32_t addrLoBE = OSSwapHostToBigInt32(meta.addressLo);
    std::memcpy(&out[4], &addrLoBE, sizeof(uint32_t));
}

} // namespace

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

    EncodeOrbAddress(loginORBAddressBE_, localNode, loginORBMeta_);
}

void LoginSession::BuildReconnectORB(uint16_t loginID) noexcept {
    std::memset(&reconnectORBBuffer_, 0, sizeof(reconnectORBBuffer_));

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    reconnectORBBuffer_.options = Options::kReconnectNotify;
    reconnectORBBuffer_.loginID = OSSwapHostToBigInt16(loginID);
    reconnectORBBuffer_.statusFIFOAddressHi =
        OSSwapHostToBigInt32(ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    reconnectORBBuffer_.statusFIFOAddressLo = OSSwapHostToBigInt32(statusBlockMeta_.addressLo);

    addrSpaceMgr_.WriteLocalData(
        this, reconnectORBHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&reconnectORBBuffer_),
                                 sizeof(reconnectORBBuffer_)});

    EncodeOrbAddress(reconnectORBAddressBE_, localNode, reconnectORBMeta_);
}

void LoginSession::BuildLogoutORB(uint16_t loginID) noexcept {
    std::memset(&logoutORBBuffer_, 0, sizeof(logoutORBBuffer_));

    const uint16_t localNode =
        NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));

    logoutORBBuffer_.options = Options::kLogoutNotify;
    logoutORBBuffer_.loginID = OSSwapHostToBigInt16(loginID);
    logoutORBBuffer_.statusFIFOAddressHi =
        OSSwapHostToBigInt32(ComposeBusAddressHi(localNode, statusBlockMeta_.addressHi));
    logoutORBBuffer_.statusFIFOAddressLo = OSSwapHostToBigInt32(statusBlockMeta_.addressLo);

    addrSpaceMgr_.WriteLocalData(
        this, logoutORBHandle_, 0,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&logoutORBBuffer_),
                                 sizeof(logoutORBBuffer_)});

    EncodeOrbAddress(logoutORBAddressBE_, localNode, logoutORBMeta_);
}

const std::array<uint8_t, 8>& LoginSession::OrbAddressFor(Fsm::Op op) const noexcept {
    switch (op) {
        case Fsm::Op::Login:     return loginORBAddressBE_;
        case Fsm::Op::Reconnect: return reconnectORBAddressBE_;
        case Fsm::Op::Logout:    return logoutORBAddressBE_;
    }
    return loginORBAddressBE_;
}

// ---------------------------------------------------------------------------
// Command-plane binding
// ---------------------------------------------------------------------------

void LoginSession::BindCommandPlane(Fsm::Op op) noexcept {
    const auto* held = Fsm::Held(fsm_);
    const auto route = Fsm::RouteOf(fsm_);
    if (!held || !route) {
        return;
    }
    BindFetchAgent(*held, *route);

    // Login enables unsolicited status before the busy-timeout write (PR #19's
    // ordering); a reconnect replays the busy timeout first.
    if (op == Fsm::Op::Login) {
        if (unsolicitedStatusRequested_) {
            unsolicitedStatusRequested_ = false;
            EnableUnsolicitedStatusNow();
        }
        ASFW_LOG_SBP2( "LoginSession: login successful — loginID=%u CBA=%04x:%08x reconnectHold=2^%u",
                 held->id, held->commandBlockAgentHi, held->commandBlockAgentLo,
                 held->reconnectHold);
        WriteBusyTimeout();
        return;
    }

    ASFW_LOG_SBP2( "LoginSession: reconnect successful — loginID=%u", held->id);
    WriteBusyTimeout();
    if (unsolicitedStatusRequested_) {
        unsolicitedStatusRequested_ = false;
        EnableUnsolicitedStatusNow();
    }
}

void LoginSession::BindFetchAgent(const Fsm::HeldLogin& login, const Fsm::Route& route) noexcept {
    const auto cbaAddr = [&](uint32_t offset) {
        return Async::FWAddress{Async::FWAddress::QualifiedAddressParts{
            .addressHi = login.commandBlockAgentHi,
            .addressLo = login.commandBlockAgentLo + offset,
            .nodeID = route.node}};
    };

    FetchAgent::Binding binding{};
    binding.generation = route.generation;
    binding.nodeID = route.node;
    binding.fetchAgentAddress = cbaAddr(Wire::CommandBlockAgentOffsets::kFetchAgent);
    binding.agentResetAddress = cbaAddr(Wire::CommandBlockAgentOffsets::kAgentReset);
    binding.maxPayloadSize = maxPayloadSize_;
    fetchAgent_.Bind(binding);

    unsolicitedStatusAddress_ = cbaAddr(Wire::CommandBlockAgentOffsets::kUnsolicitedStatusEnable);
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

    Serialized([this, block, offset, len] {
        ASFW_LOG_SBP2( "LoginSession::OnStatusBlockRemoteWrite: state=%{public}s offset=%u len=%u sbpStatus=%u",
                 Fsm::Name(fsm_), offset, len, block.sbpStatus);

        switch (Fsm::Reported(fsm_)) {
            case LoginState::LoggingIn:
                CompleteLoginFromStatusBlock(block);
                break;
            case LoginState::Reconnecting:
                CompleteReconnectFromStatusBlock(block);
                break;
            case LoginState::LoggingOut:
                ASFW_LOG_SBP2( "LoginSession: logout complete (was loginID=%u)",
                         Fsm::Held(fsm_) ? Fsm::Held(fsm_)->id : 0);
                (void)Apply(Fsm::Ev::LogoutDone{});
                break;
            case LoginState::LoggedIn:
                ProcessStatusBlock(block, len);
                break;
            default:
                ASFW_LOG_SBP2( "LoginSession: unexpected status block in state %{public}s",
                         Fsm::Name(fsm_));
                break;
        }
    });
}

void LoginSession::CompleteLoginFromStatusBlock(const Wire::StatusBlock& block) noexcept {
    const auto* loggingIn = std::get_if<Fsm::St::LoggingIn>(&fsm_);
    if (!loggingIn) {
        return;
    }

    if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        ASFW_LOG_SBP2( "LoginSession: login failed — sbpStatus=%u, retrying (%u/%u)",
                 block.sbpStatus, loggingIn->retries + 1, Fsm::kLoginRetryMax);
        (void)Apply(Fsm::Ev::LoginRejected{});
        return;
    }

    // Login succeeded — read the login response the device wrote to our space.
    std::vector<uint8_t> responseData;
    const auto kr = addrSpaceMgr_.ReadIncomingData(
        this, loginResponseHandle_, 0, sizeof(Wire::LoginResponse), &responseData);

    if (kr != kIOReturnSuccess || responseData.size() < sizeof(Wire::LoginResponse)) {
        ASFW_LOG_SBP2( "LoginSession: failed to read login response (kr=0x%08x, len=%zu)",
                 kr, responseData.size());
        (void)Apply(Fsm::Ev::LoginRejected{});
        return;
    }

    Wire::LoginResponse resp{};
    std::memcpy(&resp, responseData.data(), sizeof(resp));

    Fsm::HeldLogin login{};
    login.id = OSSwapBigToHostInt16(resp.loginID);
    login.reconnectHold = OSSwapBigToHostInt16(resp.reconnectHold);
    login.commandBlockAgentHi =
        static_cast<uint16_t>(OSSwapBigToHostInt32(resp.commandBlockAgentAddressHi) & 0xFFFFu);
    login.commandBlockAgentLo = OSSwapBigToHostInt32(resp.commandBlockAgentAddressLo);
    login.response = resp;

    const bool physicalDmaOk = EnablePhysicalDma(loggingIn->route);
    (void)Apply(Fsm::Ev::LoginAccepted{login, physicalDmaOk});
}

void LoginSession::CompleteReconnectFromStatusBlock(const Wire::StatusBlock& block) noexcept {
    const auto* reconnecting = std::get_if<Fsm::St::Reconnecting>(&fsm_);
    if (!reconnecting) {
        return;
    }
    if (block.sbpStatus != Wire::SBPStatus::kNoAdditionalInfo) {
        ASFW_LOG_SBP2( "LoginSession: reconnect failed — sbpStatus=%u, falling back to full login",
                 block.sbpStatus);
        (void)Apply(Fsm::Ev::ReconnectRejected{});
        return;
    }
    const bool physicalDmaOk = EnablePhysicalDma(reconnecting->route);
    (void)Apply(Fsm::Ev::ReconnectAccepted{physicalDmaOk});
}

// Command data moves by physical DMA, so a login is usable only once the OHCI
// admits the target's node for this generation; Linux enables at login
// (sbp2.c:1146) and reconnect (sbp2.c:1198) the same way. A stale generation
// means a reset is already in flight: wait for rediscovery like any reset.
bool LoginSession::EnablePhysicalDma(const Fsm::Route& route) noexcept {
    if (!physicalDma_) {
        return true;
    }
    const auto enabled = physicalDma_(route.node, route.generation);
    if (enabled) {
        return true;
    }
    ASFW_LOG_SBP2("LoginSession: physical DMA not enabled for node=0x%04x gen=%u (err=%u), suspending",
                  route.node, route.generation, static_cast<unsigned>(enabled.error()));
    return false;
}

void LoginSession::ProcessStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept {
    // Unsolicited: (details & 0xC0) == 0x80 (source bit set, resp == 0).
    const bool isUnsolicited = (block.details & 0xC0) == 0x80;

    if (statusCallback_) {
        statusCallback_(block, length);
    }

    if (isUnsolicited) {
        EnableUnsolicitedStatusNow();  // re-arm so the device can send more
        return;
    }

    (void)fetchAgent_.OnStatusBlock(block, length);
}

// ---------------------------------------------------------------------------
// Command plane (delegates to FetchAgent)
// ---------------------------------------------------------------------------

bool LoginSession::SubmitORB(SBP2CommandORB* orb) noexcept {
    const LoginState state = Snapshot().state;
    if (state != LoginState::LoggedIn) {
        ASFW_LOG_SBP2( "LoginSession::SubmitORB: state=%{public}s, rejecting", ToString(state));
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
    Serialized([this] { EnableUnsolicitedStatusNow(); });
}

void LoginSession::EnableUnsolicitedStatusNow() noexcept {
    const auto* loggedIn = std::get_if<Fsm::St::LoggedIn>(&fsm_);
    if (!loggedIn) {
        unsolicitedStatusRequested_ = true;
        return;
    }

    const auto route = loggedIn->route;
    const FW::NodeId node = ShortNode(route.node);
    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    unsolicitedStatusWriteHandle_ = bus_.WriteQuad(
        FW::Generation{route.generation}, node, unsolicitedStatusAddress_, 0,
        busInfo_.GetSpeed(node),
        [weakSelf, requestGeneration = route.generation](Async::AsyncStatus status,
                                                         std::span<const uint8_t>) {
            if (auto self = weakSelf.lock()) {
                self->Serialized([self, requestGeneration, status] {
                    self->OnUnsolicitedStatusEnableComplete(requestGeneration, status);
                });
            }
        });
}

void LoginSession::OnUnsolicitedStatusEnableComplete(uint16_t expectedGeneration,
                                                     Async::AsyncStatus status) noexcept {
    const auto* loggedIn = std::get_if<Fsm::St::LoggedIn>(&fsm_);
    if (!loggedIn || expectedGeneration != loggedIn->route.generation) {
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
    const auto route = Fsm::RouteOf(fsm_);
    if (!route) {
        return;
    }

    const FW::NodeId node = ShortNode(route->node);
    const Async::FWAddress busyAddr{Async::FWAddress::QualifiedAddressParts{
        .addressHi = kCSRBusAddressHi,
        .addressLo = kBusyTimeoutAddressLo,
        .nodeID = route->node}};

    busyTimeoutInProgress_ = true;
    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    busyTimeoutWriteHandle_ = bus_.WriteBlock(
        FW::Generation{route->generation}, node, busyAddr,
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&busyTimeoutBuffer_), 4},
        busInfo_.GetSpeed(node),
        [weakSelf, requestGeneration = route->generation](Async::AsyncStatus status,
                                                          std::span<const uint8_t>) {
            if (auto self = weakSelf.lock()) {
                self->Serialized([self, requestGeneration, status] {
                    self->OnBusyTimeoutComplete(requestGeneration, status);
                });
            }
        });

    if (!busyTimeoutWriteHandle_) {
        ASFW_LOG_SBP2( "LoginSession::WriteBusyTimeout: WriteBlock failed");
        busyTimeoutInProgress_ = false;
    }
}

void LoginSession::OnBusyTimeoutComplete(uint16_t expectedGeneration,
                                         Async::AsyncStatus status) noexcept {
    if (expectedGeneration != Fsm::GenerationOf(fsm_)) {
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

// A timer that fires after it was cancelled or re-armed (its work queued
// behind the transition that did so) is dropped by its arm count.
void LoginSession::ArmManagementTimer(uint64_t delayMs, Fsm::Event event) noexcept {
    CancelManagementTimer();
    const uint64_t arm = timerArm_;
    const std::weak_ptr<LoginSession> weakSelf = weak_from_this();
    managementTimerToken_ = scheduler_.ScheduleAfter(
        delayMs * 1'000'000ULL, [weakSelf, arm, event = std::move(event)]() mutable {
            if (auto self = weakSelf.lock()) {
                self->Serialized([self, arm, event = std::move(event)] {
                    if (arm != self->timerArm_) {
                        return;
                    }
                    self->managementTimerToken_ = kInvalidSchedulerToken;
                    (void)self->Apply(event);
                });
            }
        });
}

void LoginSession::CancelManagementTimer() noexcept {
    ++timerArm_;
    if (managementTimerToken_ != kInvalidSchedulerToken) {
        scheduler_.Cancel(managementTimerToken_);
        managementTimerToken_ = kInvalidSchedulerToken;
    }
}

} // namespace ASFW::Protocols::SBP2
