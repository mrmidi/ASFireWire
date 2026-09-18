// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// EfcTransport.cpp — see EfcTransport.hpp.
//
// Lock discipline: lock_ guards route_/nextSeqnum_/nextSerial_/queue_/inflight_
// and every Pending field. Bus writes, timer scheduling, and completions run
// with the lock released; a Pending that is being completed is first detached
// from inflight_ under the lock, so a stale write/timer callback that arrives
// afterwards fails the serial check and is ignored.

#include "EfcTransport.hpp"

#include "EfcResponseMailbox.hpp"

#include "../../../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Audio::Fireworks {

namespace {

constexpr uint64_t kMillisecond = 1000ULL * 1000ULL;
constexpr uint16_t kPhyIdMask = 0x3F;

class ScopedLock {
public:
    explicit ScopedLock(IOLock* lock) noexcept : lock_(lock) { IOLockLock(lock_); }
    ~ScopedLock() { IOLockUnlock(lock_); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    IOLock* lock_;
};

} // namespace

EfcTransport::EfcTransport(Async::IFireWireBusOps& busOps,
                           Async::IFireWireBusInfo& busInfo,
                           Scheduling::ITimerScheduler* timerScheduler) noexcept
    : busOps_(busOps), busInfo_(busInfo), timerScheduler_(timerScheduler),
      lock_(IOLockAlloc()) {
    if (lock_ == nullptr) {
        ASFW_LOG_ERROR(Audio, "[EFC] IOLockAlloc failed; transport disabled");
        return;
    }
}

bool EfcTransport::RegisterForResponses() noexcept {
    // shared_from_this() is only valid once an owning shared_ptr exists, which is
    // why this is not done in the constructor.
    registered_ = EfcResponseMailbox::Register(shared_from_this());
    if (!registered_) {
        ASFW_LOG_ERROR(Audio, "[EFC] no free response-mailbox slot; responses will be dropped");
    }
    return registered_;
}

EfcTransport::~EfcTransport() {
    // Nothing to unregister and nothing to wait for. The mailbox holds only a
    // weak reference, so reaching this destructor already proves no Publish() is
    // inside us -- one that were would be holding a strong reference and this
    // would not be running yet. The slot is reclaimed when it next expires.
    registered_ = false;
    CancelAll(kIOReturnAborted);
    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void EfcTransport::SetRoute(const Discovery::DeviceRouteToken& route) noexcept {
    if (lock_ == nullptr) return;
    ScopedLock guard(lock_);
    route_ = route;
}

Discovery::DeviceRouteToken EfcTransport::Route() const noexcept {
    if (lock_ == nullptr) return {};
    ScopedLock guard(lock_);
    return route_;
}

bool EfcTransport::HasInflight() const noexcept {
    if (lock_ == nullptr) return false;
    ScopedLock guard(lock_);
    return inflight_ != nullptr;
}

size_t EfcTransport::QueuedCount() const noexcept {
    if (lock_ == nullptr) return 0;
    ScopedLock guard(lock_);
    return queue_.size();
}

uint32_t EfcTransport::InflightSeqnum() const noexcept {
    if (lock_ == nullptr) return 0;
    ScopedLock guard(lock_);
    return inflight_ ? inflight_->seqnum : 0;
}

uint32_t EfcTransport::InflightAttempts() const noexcept {
    if (lock_ == nullptr) return 0;
    ScopedLock guard(lock_);
    return inflight_ ? inflight_->attempts : 0;
}

bool EfcTransport::OnEfcResponse(uint16_t sourceID, std::span<const uint8_t> payload) {
    return OnResponse(sourceID, payload);
}

void EfcTransport::Submit(Efc::Category category,
                          uint32_t command,
                          std::span<const uint32_t> params,
                          Completion completion) {
    if (lock_ == nullptr) {
        if (completion) completion(kIOReturnNotReady, Efc::Response{});
        return;
    }
    auto pending = std::make_unique<Pending>();
    pending->category = category;
    pending->command = command;
    pending->completion = std::move(completion);
    {
        ScopedLock guard(lock_);
        pending->seqnum = nextSeqnum_;
        nextSeqnum_ = Efc::NextSeqnum(nextSeqnum_);
        pending->frame = Efc::EncodeCommand(pending->seqnum, category, command, params);
        queue_.push_back(std::move(pending));
    }
    StartNext();
}

void EfcTransport::CancelAll(IOReturn status) noexcept {
    if (lock_ == nullptr) return;
    std::vector<std::unique_ptr<Pending>> drained;
    {
        ScopedLock guard(lock_);
        drained.reserve(queue_.size() + 1);
        if (inflight_) {
            drained.push_back(std::move(inflight_));
        }
        for (auto& pending : queue_) {
            drained.push_back(std::move(pending));
        }
        queue_.clear();
    }
    const Efc::Response empty{};
    for (auto& pending : drained) {
        if (!pending) continue;
        if (pending->timeout != Scheduling::kInvalidTimerToken && timerScheduler_ != nullptr) {
            timerScheduler_->Cancel(pending->timeout);
            pending->timeout = Scheduling::kInvalidTimerToken;
        }
        if (pending->writeOutstanding) {
            // Contract: the write callback still fires (kAborted); it is ignored
            // because this Pending is no longer inflight_.
            (void)busOps_.Cancel(pending->writeHandle);
            pending->writeOutstanding = false;
        }
        if (pending->completion) {
            Completion cb = std::move(pending->completion);
            cb(status, empty);
        }
    }
}

EfcTransport::SendSnapshot EfcTransport::PrepareSendLocked() noexcept {
    Pending& pending = *inflight_;
    pending.attempts += 1;
    pending.serial = ++nextSerial_;
    pending.writeOutstanding = false;
    return SendSnapshot{.serial = pending.serial,
                        .seqnum = pending.seqnum,
                        .attempt = pending.attempts,
                        .category = pending.category,
                        .command = pending.command,
                        .node = static_cast<uint8_t>(route_.nodeId & kPhyIdMask),
                        .frame = pending.frame};
}

void EfcTransport::StartNext() {
    std::optional<SendSnapshot> snapshot;
    std::unique_ptr<Pending> notReady;
    {
        ScopedLock guard(lock_);
        if (inflight_ || queue_.empty()) {
            return;
        }
        inflight_ = std::move(queue_.front());
        queue_.erase(queue_.begin());
        if (!route_ || timerScheduler_ == nullptr) {
            notReady = std::move(inflight_);
        } else {
            snapshot = PrepareSendLocked();
        }
    }
    if (notReady) {
        ASFW_LOG_ERROR(Audio, "[EFC] cannot send cat=%u cmd=%u: route/timer not ready",
                       static_cast<unsigned>(notReady->category), notReady->command);
        Complete(std::move(notReady), kIOReturnNotReady, nullptr);
        return;
    }
    if (snapshot) {
        Send(*snapshot);
    }
}

void EfcTransport::Send(const SendSnapshot& snapshot) {
    const FW::NodeId node{snapshot.node};
    const Async::FWAddress target{Async::FWAddress::AddressParts{
        .addressHi = Efc::kCommandAddressHi,
        .addressLo = Efc::kCommandAddressLo,
    }};

    ASFW_LOG(Audio, "[EFC] send node=%u seq=0x%08x cat=%u cmd=%u attempt=%u bytes=%zu",
             node.value, snapshot.seqnum, static_cast<unsigned>(snapshot.category),
             snapshot.command, snapshot.attempt, snapshot.frame.size());

    const uint64_t serial = snapshot.serial;
    std::weak_ptr<EfcTransport> alive = weak_from_this();
    const Async::AsyncHandle handle = busOps_.WriteBlock(
        busInfo_.GetGeneration(), node, target,
        std::span<const uint8_t>(snapshot.frame.data(), snapshot.frame.size()),
        busInfo_.GetSpeed(node),
        [this, alive, serial](Async::AsyncStatus status, std::span<const uint8_t>) {
            // lock(), not expired(): a hold, so the transport cannot be destroyed
            // between the check and the call.
            const auto held = alive.lock();
            if (!held) return;  // transport torn down; see ~EfcTransport
            OnWriteCompleted(serial, status);
        });

    // The bus may have completed the write synchronously (host tests do); only
    // remember the handle while the attempt is still the live one and unfinished.
    ScopedLock guard(lock_);
    if (inflight_ && inflight_->serial == serial && inflight_->timeout == Scheduling::kInvalidTimerToken &&
        inflight_->attempts == snapshot.attempt) {
        inflight_->writeHandle = handle;
        inflight_->writeOutstanding = true;
    }
}

void EfcTransport::OnWriteCompleted(uint64_t serial, Async::AsyncStatus status) {
    std::optional<SendSnapshot> resend;
    std::unique_ptr<Pending> done;
    IOReturn doneStatus = kIOReturnSuccess;
    {
        ScopedLock guard(lock_);
        if (!inflight_ || inflight_->serial != serial) {
            return;  // stale completion from a retried or cancelled attempt
        }
        inflight_->writeOutstanding = false;
        switch (status) {
            case Async::AsyncStatus::kSuccess:
                break;  // arm the response timeout below, outside the lock
            case Async::AsyncStatus::kTimeout:
            case Async::AsyncStatus::kBusyRetryExhausted:
                if (inflight_->attempts < Efc::kMaxAttempts) {
                    resend = PrepareSendLocked();
                } else {
                    done = std::move(inflight_);
                    doneStatus = kIOReturnTimeout;
                }
                break;
            case Async::AsyncStatus::kStaleGeneration:
                // Deliberate deviation from Linux (which sleeps 5 ms and retries
                // without spending an attempt): after a bus reset the route
                // token is re-issued and the duplex coordinator re-prepares, so
                // failing fast here hands recovery to the layer that owns it.
                done = std::move(inflight_);
                doneStatus = kIOReturnNotResponding;
                break;
            case Async::AsyncStatus::kAborted:
                done = std::move(inflight_);
                doneStatus = kIOReturnAborted;
                break;
            default:
                done = std::move(inflight_);
                doneStatus = kIOReturnIOError;
                break;
        }
    }
    if (status == Async::AsyncStatus::kSuccess) {
        ArmTimeout(serial);
        return;
    }
    if (resend) {
        ASFW_LOG(Audio, "[EFC] write %{public}s seq=0x%08x, retrying",
                 Async::ToString(status), resend->seqnum);
        Send(*resend);
        return;
    }
    if (done) {
        if (doneStatus != kIOReturnTimeout && doneStatus != kIOReturnAborted) {
            ASFW_LOG_ERROR(Audio, "[EFC] write failed %{public}s seq=0x%08x",
                           Async::ToString(status), done->seqnum);
        }
        Complete(std::move(done), doneStatus, nullptr);
    }
}

void EfcTransport::ArmTimeout(uint64_t serial) {
    std::weak_ptr<EfcTransport> alive = weak_from_this();
    const uint64_t timeoutNs = static_cast<uint64_t>(Efc::kTimeoutMs) * kMillisecond;
    const Scheduling::TimerToken token = timerScheduler_->ScheduleAfter(
        timeoutNs, [this, alive, serial]() {
            // lock(), not expired(): holds the transport for the callback.
            const auto held = alive.lock();
            if (!held) return;
            OnTimeout(serial);
        });

    std::unique_ptr<Pending> done;
    Scheduling::TimerToken staleToken = Scheduling::kInvalidTimerToken;
    {
        ScopedLock guard(lock_);
        if (inflight_ && inflight_->serial == serial) {
            if (token == Scheduling::kInvalidTimerToken) {
                done = std::move(inflight_);  // no timer => no way to notice a lost response
            } else {
                inflight_->timeout = token;
            }
        } else {
            staleToken = token;  // the attempt finished while we were scheduling
        }
    }
    if (staleToken != Scheduling::kInvalidTimerToken) {
        timerScheduler_->Cancel(staleToken);
    }
    if (done) {
        ASFW_LOG_ERROR(Audio, "[EFC] could not arm response timeout seq=0x%08x", done->seqnum);
        Complete(std::move(done), kIOReturnNotReady, nullptr);
    }
}

void EfcTransport::OnTimeout(uint64_t serial) {
    std::optional<SendSnapshot> resend;
    std::unique_ptr<Pending> done;
    {
        ScopedLock guard(lock_);
        if (!inflight_ || inflight_->serial != serial) {
            return;
        }
        inflight_->timeout = Scheduling::kInvalidTimerToken;
        if (inflight_->attempts < Efc::kMaxAttempts) {
            resend = PrepareSendLocked();
        } else {
            done = std::move(inflight_);
        }
    }
    if (resend) {
        ASFW_LOG(Audio, "[EFC] no response for seq=0x%08x within %u ms, retrying",
                 resend->seqnum, Efc::kTimeoutMs);
        Send(*resend);
        return;
    }
    if (done) {
        ASFW_LOG_ERROR(Audio, "[EFC] transaction timed out seq=0x%08x cat=%u cmd=%u",
                       done->seqnum, static_cast<unsigned>(done->category), done->command);
        Complete(std::move(done), kIOReturnTimeout, nullptr);
    }
}

bool EfcTransport::OnResponse(uint16_t sourceID, std::span<const uint8_t> payload) {
    if (lock_ == nullptr) return false;
    auto decoded = Efc::DecodeResponse(payload);
    if (!decoded) {
        return false;
    }
    std::unique_ptr<Pending> done;
    {
        ScopedLock guard(lock_);
        if (!inflight_ || !route_ ||
            (sourceID & kPhyIdMask) != (route_.nodeId & kPhyIdMask) ||
            decoded->header.seqnum != inflight_->seqnum + 1) {
            return false;  // someone else's transaction, another node, or a stale echo
        }
        done = std::move(inflight_);
    }
    if (done->timeout != Scheduling::kInvalidTimerToken) {
        timerScheduler_->Cancel(done->timeout);
        done->timeout = Scheduling::kInvalidTimerToken;
    }

    IOReturn status = kIOReturnSuccess;
    if (!Efc::ResponseMatches(*decoded, done->seqnum, done->category, done->command)) {
        ASFW_LOG_ERROR(Audio, "[EFC] response cat/cmd mismatch seq=0x%08x got cat=%u cmd=%u want cat=%u cmd=%u",
                       decoded->header.seqnum, decoded->header.category, decoded->header.command,
                       static_cast<unsigned>(done->category), done->command);
        status = kIOReturnBadArgument;
    } else if (decoded->header.status != static_cast<uint32_t>(Efc::Status::kOk)) {
        ASFW_LOG_ERROR(Audio, "[EFC] command failed cat=%u cmd=%u status=%u (%{public}s)",
                       decoded->header.category, decoded->header.command,
                       decoded->header.status, Efc::StatusName(decoded->header.status));
        status = kIOReturnError;
    }
    Complete(std::move(done), status, &*decoded);
    return true;
}

void EfcTransport::Complete(std::unique_ptr<Pending> done, IOReturn status, const Efc::Response* response) {
    if (!done) {
        return;
    }
    if (done->writeOutstanding) {
        (void)busOps_.Cancel(done->writeHandle);
        done->writeOutstanding = false;
    }
    const Efc::Response empty{};
    if (done->completion) {
        Completion cb = std::move(done->completion);
        cb(status, response ? *response : empty);
    }
    StartNext();
}

} // namespace ASFW::Audio::Fireworks
