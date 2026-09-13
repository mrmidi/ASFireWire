// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// EfcTransport.cpp — see EfcTransport.hpp.

#include "EfcTransport.hpp"

#include "EfcResponseMailbox.hpp"

#include "../../../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Audio::Fireworks {

namespace {

constexpr uint64_t kMillisecond = 1000ULL * 1000ULL;
constexpr uint16_t kPhyIdMask = 0x3F;

} // namespace

EfcTransport::EfcTransport(Async::IFireWireBusOps& busOps,
                           Async::IFireWireBusInfo& busInfo,
                           Scheduling::ITimerScheduler* timerScheduler) noexcept
    : busOps_(busOps), busInfo_(busInfo), timerScheduler_(timerScheduler) {
    registered_ = EfcResponseMailbox::AddObserver(this, &EfcTransport::ObserverThunk);
    if (!registered_) {
        ASFW_LOG_ERROR(Audio, "[EFC] no free response-mailbox slot; responses will be dropped");
    }
}

EfcTransport::~EfcTransport() {
    CancelAll(kIOReturnAborted);
    if (registered_) {
        EfcResponseMailbox::RemoveObserver(this);
        registered_ = false;
    }
}

void EfcTransport::SetRoute(const Discovery::DeviceRouteToken& route) noexcept {
    route_ = route;
}

uint32_t EfcTransport::InflightSeqnum() const noexcept {
    return inflight_ ? inflight_->seqnum : 0;
}

uint32_t EfcTransport::InflightAttempts() const noexcept {
    return inflight_ ? inflight_->attempts : 0;
}

bool EfcTransport::ObserverThunk(void* context, uint16_t sourceID, std::span<const uint8_t> payload) {
    return static_cast<EfcTransport*>(context)->OnResponse(sourceID, payload);
}

void EfcTransport::Submit(Efc::Category category,
                          uint32_t command,
                          std::span<const uint32_t> params,
                          Completion completion) {
    auto pending = std::make_unique<Pending>();
    pending->category = category;
    pending->command = command;
    pending->seqnum = nextSeqnum_;
    nextSeqnum_ = Efc::NextSeqnum(nextSeqnum_);
    pending->frame = Efc::EncodeCommand(pending->seqnum, category, command, params);
    pending->completion = std::move(completion);
    queue_.push_back(std::move(pending));
    StartNext();
}

void EfcTransport::CancelAll(IOReturn status) noexcept {
    std::vector<std::unique_ptr<Pending>> drained;
    drained.reserve(queue_.size() + 1);
    if (inflight_) {
        CancelTimeout(*inflight_);
        drained.push_back(std::move(inflight_));
    }
    for (auto& pending : queue_) {
        drained.push_back(std::move(pending));
    }
    queue_.clear();

    const Efc::Response empty{};
    for (auto& pending : drained) {
        if (pending && pending->completion) {
            Completion cb = std::move(pending->completion);
            cb(status, empty);
        }
    }
}

void EfcTransport::StartNext() {
    if (inflight_ || queue_.empty()) {
        return;
    }
    inflight_ = std::move(queue_.front());
    queue_.erase(queue_.begin());

    if (!route_ || timerScheduler_ == nullptr) {
        ASFW_LOG_ERROR(Audio, "[EFC] cannot send cat=%u cmd=%u: route/timer not ready",
                       static_cast<unsigned>(inflight_->category), inflight_->command);
        FinishInflight(kIOReturnNotReady, nullptr);
        return;
    }
    Send();
}

void EfcTransport::Send() {
    Pending& pending = *inflight_;
    pending.attempts += 1;
    pending.serial = ++nextSerial_;
    const uint64_t serial = pending.serial;

    const FW::NodeId node{static_cast<uint8_t>(route_.nodeId & kPhyIdMask)};
    const Async::FWAddress target{Async::FWAddress::AddressParts{
        .addressHi = Efc::kCommandAddressHi,
        .addressLo = Efc::kCommandAddressLo,
    }};

    ASFW_LOG(Audio, "[EFC] send node=%u seq=0x%08x cat=%u cmd=%u attempt=%u bytes=%zu",
             node.value, pending.seqnum, static_cast<unsigned>(pending.category),
             pending.command, pending.attempts, pending.frame.size());

    (void)busOps_.WriteBlock(busInfo_.GetGeneration(), node, target,
                             std::span<const uint8_t>(pending.frame.data(), pending.frame.size()),
                             busInfo_.GetSpeed(node),
                             [this, serial](Async::AsyncStatus status, std::span<const uint8_t>) {
                                 OnWriteCompleted(serial, status);
                             });
}

void EfcTransport::OnWriteCompleted(uint64_t serial, Async::AsyncStatus status) {
    if (!inflight_ || inflight_->serial != serial) {
        return;  // stale completion from a retried attempt
    }
    switch (status) {
        case Async::AsyncStatus::kSuccess: {
            const uint64_t timeoutNs = static_cast<uint64_t>(Efc::kTimeoutMs) * kMillisecond;
            inflight_->timeout = timerScheduler_->ScheduleAfter(
                timeoutNs, [this, serial]() { OnTimeout(serial); });
            return;
        }
        case Async::AsyncStatus::kTimeout:
        case Async::AsyncStatus::kBusyRetryExhausted:
            if (inflight_->attempts < Efc::kMaxAttempts) {
                ASFW_LOG(Audio, "[EFC] write %{public}s seq=0x%08x, retrying",
                         Async::ToString(status), inflight_->seqnum);
                Send();
                return;
            }
            FinishInflight(kIOReturnTimeout, nullptr);
            return;
        case Async::AsyncStatus::kStaleGeneration:
            FinishInflight(kIOReturnNotResponding, nullptr);
            return;
        case Async::AsyncStatus::kAborted:
            FinishInflight(kIOReturnAborted, nullptr);
            return;
        default:
            ASFW_LOG_ERROR(Audio, "[EFC] write failed %{public}s seq=0x%08x",
                           Async::ToString(status), inflight_->seqnum);
            FinishInflight(kIOReturnIOError, nullptr);
            return;
    }
}

void EfcTransport::OnTimeout(uint64_t serial) {
    if (!inflight_ || inflight_->serial != serial) {
        return;
    }
    inflight_->timeout = Scheduling::kInvalidTimerToken;
    if (inflight_->attempts < Efc::kMaxAttempts) {
        ASFW_LOG(Audio, "[EFC] no response for seq=0x%08x within %u ms, retrying",
                 inflight_->seqnum, Efc::kTimeoutMs);
        Send();
        return;
    }
    ASFW_LOG_ERROR(Audio, "[EFC] transaction timed out seq=0x%08x cat=%u cmd=%u",
                   inflight_->seqnum, static_cast<unsigned>(inflight_->category),
                   inflight_->command);
    FinishInflight(kIOReturnTimeout, nullptr);
}

bool EfcTransport::SourceMatches(uint16_t sourceID) const noexcept {
    return route_ && (sourceID & kPhyIdMask) == (route_.nodeId & kPhyIdMask);
}

bool EfcTransport::OnResponse(uint16_t sourceID, std::span<const uint8_t> payload) {
    if (!inflight_ || !SourceMatches(sourceID)) {
        return false;
    }
    auto decoded = Efc::DecodeResponse(payload);
    if (!decoded) {
        return false;
    }
    if (decoded->header.seqnum != inflight_->seqnum + 1) {
        return false;  // someone else's transaction (or a stale retry echo)
    }

    CancelTimeout(*inflight_);

    if (!Efc::ResponseMatches(*decoded, inflight_->seqnum, inflight_->category, inflight_->command)) {
        ASFW_LOG_ERROR(Audio, "[EFC] response cat/cmd mismatch seq=0x%08x got cat=%u cmd=%u want cat=%u cmd=%u",
                       decoded->header.seqnum, decoded->header.category, decoded->header.command,
                       static_cast<unsigned>(inflight_->category), inflight_->command);
        FinishInflight(kIOReturnBadArgument, &*decoded);
        return true;
    }
    if (decoded->header.status != static_cast<uint32_t>(Efc::Status::kOk)) {
        ASFW_LOG_ERROR(Audio, "[EFC] command failed cat=%u cmd=%u status=%u (%{public}s)",
                       decoded->header.category, decoded->header.command,
                       decoded->header.status, Efc::StatusName(decoded->header.status));
        FinishInflight(kIOReturnError, &*decoded);
        return true;
    }
    FinishInflight(kIOReturnSuccess, &*decoded);
    return true;
}

void EfcTransport::CancelTimeout(Pending& pending) noexcept {
    if (pending.timeout != Scheduling::kInvalidTimerToken && timerScheduler_ != nullptr) {
        timerScheduler_->Cancel(pending.timeout);
    }
    pending.timeout = Scheduling::kInvalidTimerToken;
}

void EfcTransport::FinishInflight(IOReturn status, const Efc::Response* response) {
    std::unique_ptr<Pending> done = std::move(inflight_);
    if (!done) {
        return;
    }
    CancelTimeout(*done);
    const Efc::Response empty{};
    if (done->completion) {
        Completion cb = std::move(done->completion);
        cb(status, response ? *response : empty);
    }
    StartNext();
}

} // namespace ASFW::Audio::Fireworks
