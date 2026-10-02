//
// FCPTransport.cpp
// ASFWDriver - AV/C Protocol Layer
//
// The AV/C transaction engine. See FCPTransport.hpp for the phases.
//

#include "FCPTransport.hpp"
#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <utility>

using namespace ASFW::Protocols::AVC;

namespace {

using ASFW::AVC::AvcError;
using ASFW::AVC::AvcErrorKind;

[[nodiscard]] AvcError ErrorOf(AvcErrorKind kind) noexcept { return AvcError::Of(kind); }

[[nodiscard]] FcpExchangeOutcome OutcomeFor(const ASFW::AVC::Expected<FCPFrame>& result) noexcept {
    if (result) {
        return FcpExchangeOutcome::kResponse;
    }
    switch (result.error().kind) {
        case AvcErrorKind::kTimeout: return FcpExchangeOutcome::kTimeout;
        case AvcErrorKind::kBusReset: return FcpExchangeOutcome::kBusReset;
        case AvcErrorKind::kRefused: return FcpExchangeOutcome::kRefusedByFilter;
        case AvcErrorKind::kBusy: return FcpExchangeOutcome::kBusy;
        case AvcErrorKind::kFrameTooShort:
        case AvcErrorKind::kFrameTooLong: return FcpExchangeOutcome::kInvalid;
        default: return FcpExchangeOutcome::kTransportError;
    }
}

[[nodiscard]] FCPFrame FrameOf(std::span<const uint8_t> bytes) noexcept {
    FCPFrame frame;
    frame.length = std::min(bytes.size(), frame.data.size());
    std::copy_n(bytes.begin(), frame.length, frame.data.begin());
    return frame;
}

} // namespace

//==============================================================================
// Lifecycle
//==============================================================================

bool FCPTransport::init(Protocols::Ports::FireWireBusOps* busOps,
                        Protocols::Ports::FireWireBusInfo* busInfo,
                        Discovery::FWDevice* device,
                        Discovery::DeviceRegistry& routeRegistry,
                        Scheduling::ITimerScheduler& timerScheduler,
                        const FCPTransportConfig& config) {
    busOps_ = busOps;
    busInfo_ = busInfo;
    device_ = device;
    routeRegistry_ = &routeRegistry;
    timerScheduler_ = &timerScheduler;
    config_ = config;
    shuttingDown_ = false;

    if (!busOps_ || !busInfo_ || !device_) {
        ASFW_LOG_V1(FCP, "FCPTransport: Missing bus port or target device");
        return false;
    }
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to allocate lock");
        return false;
    }
    // The attach-time discovery is the first session.
    recorder_.BeginSession();
    ASFW_LOG_V1(FCP, "FCPTransport: Initialized for device nodeID=%u, cmdAddr=0x%llx, rspAddr=0x%llx",
                device_->GetNodeID(), config_.commandAddress, config_.responseAddress);
    return true;
}

FCPTransport::~FCPTransport() {
    Shutdown();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void FCPTransport::Shutdown() {
    if (!lock_) {
        shuttingDown_ = true;
        return;
    }
    std::deque<Delivery> failed;
    Async::AsyncHandle handle{};

    IOLockLock(lock_);
    if (shuttingDown_) {
        IOLockUnlock(lock_);
        return;
    }
    shuttingDown_ = true;
    if (active_) {
        if (const auto* writing = std::get_if<Writing>(&active_->phase)) {
            handle = writing->handle;
        }
        failed.push_back(Finish(std::unexpected(ErrorOf(AvcErrorKind::kTransportError))));
    }
    while (!queue_.empty()) {
        failed.push_back(Delivery{std::move(queue_.front()), std::unexpected(ErrorOf(AvcErrorKind::kTransportError))});
        queue_.pop_front();
    }
    IOLockUnlock(lock_);

    if (handle.value && busOps_) {
        busOps_->Cancel(handle);
    }
    for (auto& delivery : failed) {
        Deliver(std::move(delivery));
    }
}

//==============================================================================
// Submission
//==============================================================================

void FCPTransport::Submit(const ASFW::AVC::CommandFrame& frame,
                          FW::Generation generation,
                          ResponseCallback completion) {
    const auto type = frame.Type();
    Transaction txn{
        .frame = frame,
        .completion = std::move(completion),
        .idempotent = type == ASFW::AVC::CommandType::kStatus ||
                      type == ASFW::AVC::CommandType::kSpecificInquiry ||
                      type == ASFW::AVC::CommandType::kGeneralInquiry,
        .generation = generation,
    };
    txn.retriesLeft = txn.idempotent ? config_.maxRetries : 0;

    const auto wire = frame.WireBytes();
    const auto refuse = [this, &txn](AvcErrorKind kind) {
        Result result = std::unexpected(ErrorOf(kind));
        if (lock_) {
            IOLockLock(lock_);
            Record(txn, result);
            IOLockUnlock(lock_);
        }
        Deliver(Delivery{std::move(txn), std::move(result)});
    };

    if (wire.size() > kAVCFrameMaxSize) {
        refuse(AvcErrorKind::kFrameTooLong);
        return;
    }
    // Every AV/C frame this driver sends passes through here, including the
    // user-client raw path whose payload comes from user space. Devices whose
    // firmware hangs on unimplemented AV/C carry a non-empty allowlist.
    if (!FrameIsPermitted(config_.permittedFrames, wire)) {
        ASFW_LOG_ERROR(FCP, "FCPTransport: refused ctype=0x%02x opcode=0x%02x — not in this device's permitted command set",
                       wire[0], wire[2]);
        refuse(AvcErrorKind::kRefused);
        return;
    }
    if (!lock_) {
        Deliver(Delivery{std::move(txn), std::unexpected(ErrorOf(AvcErrorKind::kTransportError))});
        return;
    }

    IOLockLock(lock_);
    if (shuttingDown_) {
        IOLockUnlock(lock_);
        Deliver(Delivery{std::move(txn), std::unexpected(ErrorOf(AvcErrorKind::kTransportError))});
        return;
    }
    const auto route = routeRegistry_ ? routeRegistry_->CurrentRoute(device_->GetGUID()) : std::nullopt;
    if (!route || route->generation != generation) {
        IOLockUnlock(lock_);
        refuse(AvcErrorKind::kBusReset);
        return;
    }
    txn.id = ++nextTransactionID_;
    if (txn.id == 0) {
        txn.id = ++nextTransactionID_;
    }
    if (active_ || !queue_.empty()) {
        queue_.push_back(std::move(txn));
        IOLockUnlock(lock_);
        return;
    }
    IOLockUnlock(lock_);
    Start(std::move(txn));
}

void FCPTransport::Start(Transaction txn) {
    IOLockLock(lock_);
    if (shuttingDown_) {
        IOLockUnlock(lock_);
        Deliver(Delivery{std::move(txn), std::unexpected(ErrorOf(AvcErrorKind::kTransportError))});
        return;
    }
    txn.startedNs = timerScheduler_ ? timerScheduler_->NowNs() : 0;
    active_.emplace(Active{.txn = std::move(txn), .phase = Writing{}});
    IOLockUnlock(lock_);
    IssueWrite();
}

void FCPTransport::StartNext() {
    IOLockLock(lock_);
    if (shuttingDown_ || active_ || queue_.empty()) {
        IOLockUnlock(lock_);
        return;
    }
    Transaction next = std::move(queue_.front());
    queue_.pop_front();
    IOLockUnlock(lock_);
    Start(std::move(next));
}

//==============================================================================
// Writing
//==============================================================================

void FCPTransport::IssueWrite() {
    IOLockLock(lock_);
    if (shuttingDown_ || !active_) {
        IOLockUnlock(lock_);
        return;
    }
    // A command built for one generation never reaches another: the unit may
    // be a different node now.
    const auto route = routeRegistry_ ? routeRegistry_->CurrentRoute(device_->GetGUID()) : std::nullopt;
    if (!route || route->generation != active_->txn.generation) {
        Delivery stale = Finish(std::unexpected(ErrorOf(AvcErrorKind::kBusReset)));
        IOLockUnlock(lock_);
        Deliver(std::move(stale));
        return;
    }
    const WriteAttempt attempt{.id = ++nextWriteAttempt_, .route = *route};
    active_->phase = Writing{.attempt = attempt};
    const uint32_t id = active_->txn.id;
    const FCPFrame command = FrameOf(active_->txn.frame.WireBytes());
    IOLockUnlock(lock_);

    // The async transaction owns this transport until its callback leaves.
    const auto self = weak_from_this().lock();
    if (!self) {
        FinishIfActive(id, std::unexpected(ErrorOf(AvcErrorKind::kTransportError)));
        return;
    }
    const Async::FWAddress address{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((config_.commandAddress >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(config_.commandAddress & 0xFFFFFFFFU),
    }};
    ASFW_LOG_HEX(FCP, "FCPTransport: write attempt=%llu node=0x%04x gen=%u ctype=0x%02x opcode=0x%02x len=%zu",
                 attempt.id, attempt.route.nodeId, attempt.route.generation.value,
                 command.data[0], command.data[2], command.length);
    const auto handle = busOps_->WriteBlock(
        FW::Generation{attempt.route.generation.value},
        FW::NodeId{static_cast<uint8_t>(attempt.route.nodeId & 0x3Fu)},
        address, command.Payload(), FW::FwSpeed::S100,
        [self, attempt](Async::AsyncStatus status, std::span<const uint8_t>) {
            self->OnWriteComplete(attempt, status);
        });

    IOLockLock(lock_);
    auto* writing = active_ && active_->txn.id == id ? std::get_if<Writing>(&active_->phase) : nullptr;
    const bool current = writing != nullptr && writing->attempt.id == attempt.id;
    if (!handle.value) {
        if (!current) {
            IOLockUnlock(lock_);
            return;
        }
        // Admission failed at once. A route that moved on meanwhile is a reset.
        const auto now = routeRegistry_ ? routeRegistry_->CurrentRoute(device_->GetGUID()) : std::nullopt;
        const bool stale = !now || now->generation != attempt.route.generation;
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to submit async write");
        Delivery failed = Finish(std::unexpected(ErrorOf(stale ? AvcErrorKind::kBusReset
                                                               : AvcErrorKind::kTransportError)));
        IOLockUnlock(lock_);
        Deliver(std::move(failed));
        return;
    }
    if (current) {
        writing->handle = handle;
    }
    IOLockUnlock(lock_);
    if (!current && busOps_) {
        busOps_->Cancel(handle);
    }
}

void FCPTransport::OnWriteComplete(WriteAttempt attempt, Async::AsyncStatus status) {
    IOLockLock(lock_);
    // Only the write in flight counts: a response may already have answered it,
    // and a replay or reset may have replaced it.
    auto* writing = active_ && !shuttingDown_ ? std::get_if<Writing>(&active_->phase) : nullptr;
    if (writing == nullptr || writing->attempt.id != attempt.id ||
        !routeRegistry_ || !routeRegistry_->IsCurrent(attempt.route)) {
        IOLockUnlock(lock_);
        return;
    }
    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG_V1(FCP, "FCPTransport: Async write failed: %{public}s", ASFW::Async::ToString(status));
        if (active_->txn.idempotent && active_->txn.retriesLeft > 0) {
            IOLockUnlock(lock_);
            Replay();
            return;
        }
        Delivery failed = Finish(std::unexpected(ErrorOf(AvcErrorKind::kTransportError)));
        IOLockUnlock(lock_);
        Deliver(std::move(failed));
        return;
    }
    // The response deadline starts when the command reached the target; Apple
    // arms its timer from writeDone (IOFireWireAVCCommand.cpp:76-90).
    active_->phase = AwaitingResponse{.attempt = attempt};
    ArmTimer(config_.timeoutMs);
    IOLockUnlock(lock_);
}

void FCPTransport::Replay() {
    IOLockLock(lock_);
    if (shuttingDown_ || !active_ || !active_->txn.idempotent || active_->txn.retriesLeft == 0) {
        IOLockUnlock(lock_);
        return;
    }
    --active_->txn.retriesLeft;
    DisarmTimer();
    ASFW_LOG_V2(FCP, "FCPTransport: Replaying command id=%u (%u replays left)",
                active_->txn.id, active_->txn.retriesLeft);
    IOLockUnlock(lock_);
    IssueWrite();
}

//==============================================================================
// Responses
//==============================================================================

std::optional<FCPTransport::WriteAttempt> FCPTransport::AttemptOf(const Phase& phase) noexcept {
    if (const auto* writing = std::get_if<Writing>(&phase)) {
        return writing->attempt;
    }
    if (const auto* awaiting = std::get_if<AwaitingResponse>(&phase)) {
        return awaiting->attempt;
    }
    return std::nullopt;
}

bool FCPTransport::ResponseMatches(const Transaction& txn, std::span<const uint8_t> response) {
    if (response.size() < kAVCFrameMinSize || response.size() > kAVCFrameMaxSize) {
        return false;
    }
    const auto command = txn.frame.Bytes();
    if (response[1] != command[1]) {
        return false;
    }
    // The tape subunit answers TRANSPORT STATE with the current transport mode
    // as the opcode (IOFireWireAVCCommand.cpp:136-140).
    if ((command[1] & 0xF8) == 0x20 && command[2] == 0xD0) {
        const uint8_t opcode = response[2];
        return opcode == 0xD0 || opcode == 0xC1 || opcode == 0xC2 || opcode == 0xC3 || opcode == 0xC4;
    }
    return (response[2] & 0x7F) == (command[2] & 0x7F);
}

void FCPTransport::OnFCPResponse(uint16_t srcNodeID,
                                 uint32_t generation,
                                 std::span<const uint8_t> payload) {
    if (!lock_) {
        return;
    }
    IOLockLock(lock_);
    // A response is accepted while the command write is in flight too: the AR
    // request context is drained before AR response, so the target's answer can
    // be seen before our write completion.
    const auto attempt = active_ && !shuttingDown_ ? AttemptOf(active_->phase) : std::nullopt;
    if (!attempt) {
        IOLockUnlock(lock_);
        ASFW_LOG_V3(FCP, "FCPTransport: Spurious response (no command awaiting one)");
        return;
    }
    const uint16_t expected = attempt->route.nodeId;
    const bool nodeMatches = srcNodeID == expected || (srcNodeID & 0x3F) == (expected & 0x3F);
    if (!routeRegistry_ || !routeRegistry_->IsCurrent(attempt->route) || !nodeMatches ||
        generation != attempt->route.generation.value || !ResponseMatches(active_->txn, payload)) {
        IOLockUnlock(lock_);
        ASFW_LOG_V3(FCP, "FCPTransport: Ignoring response from node 0x%04x gen=%u (not for the active command)",
                    srcNodeID, generation);
        return;
    }

    if (payload[0] == static_cast<uint8_t>(ASFW::AVC::ResponseCode::kInterim)) {
        active_->txn.sawInterim = true;
        active_->phase = AwaitingResponse{.attempt = *attempt};
        ArmTimer(config_.interimTimeoutMs);
        IOLockUnlock(lock_);
        ASFW_LOG_V2(FCP, "FCPTransport: INTERIM; waiting up to %u ms for the final response",
                    config_.interimTimeoutMs);
        return;
    }

    // The response arrived as the target's block write, and this runs inside
    // the receive handler: our write response to it goes out only after the
    // handler returns. Completing here would let the completion submit the next
    // command first, while the target is still inside its response transaction
    // (TA 2004006 AV/C General 4.2 §6.5, Figure 13). A Phase 88 does not ack a
    // command written then, and wedges after a few. Linux's response handler
    // only wakes the waiting caller (sound/firewire/fcp.c:338-374) and Apple's
    // completes a command whose caller resumes on another thread
    // (IOFireWireAVCCommand.cpp:171-181), so both acknowledge the response
    // before the next command. Accept it now; deliver it from the work queue.
    active_->phase = Answered{};
    DisarmTimer();
    const uint32_t id = active_->txn.id;
    IOLockUnlock(lock_);

    const FCPFrame response = FrameOf(payload);
    const auto self = weak_from_this().lock();
    if (self && timerScheduler_) {
        const auto token = timerScheduler_->ScheduleAfter(0, [self, id, response] {
            self->FinishIfActive(id, response);
        });
        if (token != Scheduling::kInvalidTimerToken) {
            return;
        }
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to defer response delivery; completing inline");
    }
    FinishIfActive(id, response);
}

//==============================================================================
// Timeouts
//==============================================================================

void FCPTransport::ArmTimer(uint32_t timeoutMs) {
    DisarmTimer();
    const uint64_t epoch = ++nextTimerEpoch_;
    active_->timerEpoch = epoch;
    const auto self = weak_from_this().lock();
    if (!self || !timerScheduler_) {
        return;
    }
    active_->timer = timerScheduler_->ScheduleAfter(
        static_cast<uint64_t>(timeoutMs) * 1'000'000ULL, [self, epoch] { self->OnTimeout(epoch); });
    if (active_->timer == Scheduling::kInvalidTimerToken) {
        ASFW_LOG_V1(FCP, "FCPTransport: Failed to schedule the response deadline");
    }
}

void FCPTransport::DisarmTimer() {
    if (!active_) {
        return;
    }
    ++nextTimerEpoch_;
    active_->timerEpoch = 0;
    if (active_->timer != Scheduling::kInvalidTimerToken) {
        timerScheduler_->Cancel(active_->timer);
        active_->timer = Scheduling::kInvalidTimerToken;
    }
}

void FCPTransport::OnTimeout(uint64_t epoch) {
    IOLockLock(lock_);
    if (shuttingDown_ || !active_ || active_->timerEpoch != epoch ||
        !std::holds_alternative<AwaitingResponse>(active_->phase)) {
        IOLockUnlock(lock_);
        return;
    }
    active_->timer = Scheduling::kInvalidTimerToken;
    active_->timerEpoch = 0;
    ASFW_LOG_V1(FCP, "FCPTransport: Command timeout (interim=%d, replays left=%u)",
                active_->txn.sawInterim ? 1 : 0, active_->txn.retriesLeft);
    // Apple replays on a lost response (IOFireWireAVCCommand.cpp:242-254); a
    // CONTROL is not replayed here: it may already have changed the device.
    if (active_->txn.idempotent && active_->txn.retriesLeft > 0) {
        IOLockUnlock(lock_);
        Replay();
        return;
    }
    Delivery timedOut = Finish(std::unexpected(ErrorOf(AvcErrorKind::kTimeout)));
    IOLockUnlock(lock_);
    Deliver(std::move(timedOut));
}

//==============================================================================
// Bus reset
//==============================================================================

void FCPTransport::OnBusReset(uint32_t newGeneration) {
    if (!lock_) {
        return;
    }
    IOLockLock(lock_);
    // An answered command's response belongs to the generation it arrived in;
    // its delivery is already queued.
    if (shuttingDown_ || !active_ || std::holds_alternative<Answered>(active_->phase) ||
        std::holds_alternative<AwaitingRoute>(active_->phase)) {
        IOLockUnlock(lock_);
        return;
    }
    const auto attempt = AttemptOf(active_->phase);
    Async::AsyncHandle handle{};
    if (const auto* writing = std::get_if<Writing>(&active_->phase)) {
        handle = writing->handle;
    }
    DisarmTimer();
    ASFW_LOG_V2(FCP, "FCPTransport: Bus reset during command id=%u (gen %u -> %u)",
                active_->txn.id, attempt ? attempt->route.generation.value : 0U, newGeneration);

    if (config_.allowBusResetRetry && active_->txn.idempotent && active_->txn.retriesLeft > 0) {
        // Linux retries a pending transaction after a reset (fcp.c:292-317).
        // DeviceManager invalidates every route at that point, so the replay
        // waits until discovery has bound this GUID to the new generation.
        active_->phase = AwaitingRoute{.resetRoute = attempt ? std::optional{attempt->route} : std::nullopt};
        IOLockUnlock(lock_);
        if (handle.value && busOps_) {
            busOps_->Cancel(handle);
        }
        return;
    }
    Delivery reset = Finish(std::unexpected(ErrorOf(AvcErrorKind::kBusReset)));
    IOLockUnlock(lock_);
    if (handle.value && busOps_) {
        busOps_->Cancel(handle);
    }
    Deliver(std::move(reset));
}

void FCPTransport::OnRouteRevalidated(const Discovery::DeviceRouteToken& route) {
    if (!lock_) {
        return;
    }
    IOLockLock(lock_);
    const auto* waiting = active_ && !shuttingDown_ ? std::get_if<AwaitingRoute>(&active_->phase) : nullptr;
    if (waiting == nullptr || !routeRegistry_ || !routeRegistry_->IsCurrent(route) ||
        !waiting->resetRoute || route.deviceIncarnation != waiting->resetRoute->deviceIncarnation) {
        IOLockUnlock(lock_);
        return;
    }
    // Only this explicitly configured replay moves a command to a new
    // generation; every other stale submission fails with kBusReset.
    active_->txn.generation = route.generation;
    --active_->txn.retriesLeft;
    IOLockUnlock(lock_);
    ASFW_LOG_V2(FCP, "FCPTransport: Replaying command on revalidated route epoch=%llu", route.routeEpoch);
    IssueWrite();
}

//==============================================================================
// Completion
//==============================================================================

FCPTransport::Delivery FCPTransport::Finish(Result result) {
    DisarmTimer();
    Record(active_->txn, result);
    Delivery delivery{std::move(active_->txn), std::move(result)};
    active_.reset();
    return delivery;
}

void FCPTransport::FinishIfActive(uint32_t id, Result result) {
    IOLockLock(lock_);
    if (!active_ || active_->txn.id != id) {
        IOLockUnlock(lock_);
        return;
    }
    Delivery delivery = Finish(std::move(result));
    IOLockUnlock(lock_);
    Deliver(std::move(delivery));
}

void FCPTransport::Record(const Transaction& txn, const Result& result) {
    const uint8_t replays = txn.idempotent && config_.maxRetries >= txn.retriesLeft
                                ? static_cast<uint8_t>(config_.maxRetries - txn.retriesLeft)
                                : 0;
    const auto command = txn.frame.WireBytes();
    // Never-sent exchanges (refused, busy) have no duration.
    const uint64_t elapsedNs = txn.startedNs && timerScheduler_ ? timerScheduler_->NowNs() - txn.startedNs : 0;
    recorder_.Record(CurrentGeneration().value, OutcomeFor(result), txn.sawInterim, replays,
                     command.first(std::min(command.size(), kAVCFrameMaxSize)),
                     result ? result->Payload() : std::span<const uint8_t>{}, elapsedNs);
}

void FCPTransport::Deliver(Delivery delivery) {
    if (!lock_) {
        Invoke(delivery);
        return;
    }
    IOLockLock(lock_);
    deliveries_.push_back(std::move(delivery));
    if (delivering_) {
        IOLockUnlock(lock_);
        return;
    }
    delivering_ = true;
    IOLockUnlock(lock_);

    // A completion may drop the last owner of this transport (unit teardown);
    // keep it alive until the loop leaves.
    const auto self = weak_from_this().lock();
    for (;;) {
        IOLockLock(lock_);
        if (deliveries_.empty()) {
            delivering_ = false;
            IOLockUnlock(lock_);
            return;
        }
        Delivery next = std::move(deliveries_.front());
        deliveries_.pop_front();
        IOLockUnlock(lock_);

        Invoke(next);
        StartNext();
    }
}

void FCPTransport::Invoke(Delivery& delivery) {
    if (!delivery.txn.completion) {
        return;
    }
    if (!delivery.result) {
        delivery.txn.completion(std::unexpected(delivery.result.error()));
        return;
    }
    // The parsed Response views the frame held by `delivery` for this call.
    delivery.txn.completion(ASFW::AVC::ParseResponseFor(delivery.txn.frame, delivery.result->Payload()));
}

//==============================================================================
// Identity and exchange log
//==============================================================================

ASFW::FW::NodeId FCPTransport::NodeId() const noexcept {
    return ASFW::FW::NodeId{static_cast<uint8_t>(device_ ? device_->GetNodeID() : 0)};
}

ASFW::FW::Generation FCPTransport::CurrentGeneration() const noexcept {
    return busInfo_ ? busInfo_->GetGeneration() : ASFW::FW::Generation{0};
}

uint64_t FCPTransport::Guid() const noexcept {
    return device_ ? device_->GetGUID() : 0;
}

void FCPTransport::BeginExchangeSession() {
    if (!lock_) {
        return;
    }
    IOLockLock(lock_);
    recorder_.BeginSession();
    IOLockUnlock(lock_);
}

FcpExchangeLog FCPTransport::CopyExchangeLog() const {
    if (!lock_) {
        return {};
    }
    IOLockLock(lock_);
    FcpExchangeLog copy = recorder_.Log();
    IOLockUnlock(lock_);
    return copy;
}
