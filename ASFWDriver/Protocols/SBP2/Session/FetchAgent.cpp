// FetchAgent — SBP-2 command-plane engine. See FetchAgent.hpp.
// Ported from SBP2LoginSession's fetch-agent methods (PR #19), adapted to DICE:
// driven by an explicit Binding instead of login state, ORB timeouts via the
// injected ISessionScheduler (no IOSleep-on-queue), CommandORB kern_return_t.

#include "FetchAgent.hpp"

#include "../../../Common/FWCommon.hpp"

#include <algorithm>
#include <utility>

namespace ASFW::Protocols::SBP2 {

using namespace ASFW::Protocols::SBP2::Wire;

namespace {
// Fetch-agent write retry backoff, matching PR #19 (1000 ms between attempts).
constexpr uint64_t kFetchAgentWriteRetryDelayNs = 1'000'000'000ULL;
}

FetchAgent::FetchAgent(Async::IFireWireBus& bus,
                       Async::IFireWireBusInfo& busInfo,
                       ISessionScheduler& scheduler) noexcept
    : bus_(bus)
    , busInfo_(busInfo)
    , scheduler_(scheduler) {}

FetchAgent::~FetchAgent() {
    lifetimeToken_.reset();
    Clear(true);
}

// ---------------------------------------------------------------------------
// Binding lifecycle
// ---------------------------------------------------------------------------

void FetchAgent::Bind(const Binding& binding) noexcept {
    Clear(true);
    binding_ = binding;
    bound_ = true;
}

void FetchAgent::Unbind() noexcept {
    Clear(true);
    bound_ = false;
    binding_ = {};
}

void FetchAgent::Clear(bool cancelTimers) noexcept {
    const bool cancelFetchWrite =
        cancelTimers && fetchAgentWriteInUse_ && static_cast<bool>(fetchAgentWriteHandle_);
    const Async::AsyncHandle fetchWrite = fetchAgentWriteHandle_;

    if (cancelTimers) {
        for (auto& [key, entry] : outstandingORBs_) {
            if (entry.timeoutToken != kInvalidSchedulerToken) {
                scheduler_.Cancel(entry.timeoutToken);
            }
            if (entry.orb != nullptr) {
                entry.orb->SetAppended(false);
            }
        }
    }

    outstandingORBs_.clear();
    pendingImmediateORBs_.clear();
    activeFetchAgentORB_ = nullptr;
    fetchAgentWriteHandle_ = {};
    fetchAgentWriteInUse_ = false;

    if (cancelFetchWrite) {
        (void)bus_.Cancel(fetchWrite);
    }
}

// ---------------------------------------------------------------------------
// Submission
// ---------------------------------------------------------------------------

bool FetchAgent::Submit(SBP2CommandORB* orb) noexcept {
    if (!bound_) {
        ASFW_LOG(Async, "FetchAgent::Submit: not bound, rejecting");
        return false;
    }
    if (orb == nullptr || !orb->IsValid() || orb->IsAppended()) {
        ASFW_LOG(Async, "FetchAgent::Submit: invalid ORB (null=%d valid=%d appended=%d)",
                 orb == nullptr,
                 orb != nullptr && orb->IsValid(),
                 orb != nullptr && orb->IsAppended());
        return false;
    }

    const kern_return_t prepareKr =
        orb->PrepareForExecution(LocalBusNodeID(), TargetSpeed(), MaxPayloadLog());
    if (prepareKr != kIOReturnSuccess) {
        ASFW_LOG(Async, "FetchAgent::Submit: PrepareForExecution failed: 0x%08x", prepareKr);
        return false;
    }

    orb->SetFetchAgentWriteRetries(defaultWriteRetries_);
    orb->SetAppended(true);
    outstandingORBs_[MakeORBKey(orb->GetORBAddress())] = Outstanding{orb, kInvalidSchedulerToken};

    if (fetchAgentWriteInUse_) {
        pendingImmediateORBs_.push_back(orb);
        return true;
    }
    return AppendImmediate(orb);
}

bool FetchAgent::AppendImmediate(SBP2CommandORB* orb) noexcept {
    if (orb == nullptr || fetchAgentWriteInUse_) {
        return false;
    }

    const uint16_t localNode = LocalBusNodeID();
    const Async::FWAddress orbAddr = orb->GetORBAddress();
    fetchAgentWriteData_[0] = static_cast<uint8_t>(localNode >> 8);
    fetchAgentWriteData_[1] = static_cast<uint8_t>(localNode & 0xFF);
    fetchAgentWriteData_[2] = static_cast<uint8_t>(orbAddr.addressHi >> 8);
    fetchAgentWriteData_[3] = static_cast<uint8_t>(orbAddr.addressHi & 0xFF);
    const uint32_t addrLoBE = OSSwapHostToBigInt32(orbAddr.addressLo);
    std::memcpy(&fetchAgentWriteData_[4], &addrLoBE, sizeof(uint32_t));

    activeFetchAgentORB_ = orb;
    fetchAgentWriteInUse_ = true;

    const std::weak_ptr<int> weak = lifetimeToken_;
    const uint16_t requestGeneration = binding_.generation;
    fetchAgentWriteHandle_ = bus_.WriteBlock(
        FW::Generation{binding_.generation},
        FW::NodeId{static_cast<uint8_t>(binding_.nodeID & 0x3Fu)},
        binding_.fetchAgentAddress,
        std::span<const uint8_t>{fetchAgentWriteData_.data(), fetchAgentWriteData_.size()},
        TargetSpeed(),
        [this, weak, requestGeneration](Async::AsyncStatus status, std::span<const uint8_t>) {
            if (weak.expired()) {
                return;
            }
            OnFetchAgentWriteComplete(requestGeneration, status);
        });

    if (!fetchAgentWriteHandle_) {
        ASFW_LOG(Async, "FetchAgent::AppendImmediate: WriteBlock failed");
        fetchAgentWriteInUse_ = false;
        activeFetchAgentORB_ = nullptr;
        FailORB(orb, -1, Wire::SBPStatus::kUnspecifiedError);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Completions
// ---------------------------------------------------------------------------

void FetchAgent::OnFetchAgentWriteComplete(uint16_t expectedGeneration,
                                           Async::AsyncStatus status) noexcept {
    if (!bound_ || expectedGeneration != binding_.generation) {
        return;
    }

    fetchAgentWriteInUse_ = false;
    fetchAgentWriteHandle_ = {};

    if (status != Async::AsyncStatus::kSuccess) {
        ASFW_LOG(Async, "FetchAgent: ORB_POINTER write failed status=%d",
                 static_cast<int>(status));
        if (activeFetchAgentORB_ != nullptr) {
            uint32_t retries = activeFetchAgentORB_->GetFetchAgentWriteRetries();
            if (retries > 0) {
                activeFetchAgentORB_->SetFetchAgentWriteRetries(retries - 1);
                SBP2CommandORB* retryORB = activeFetchAgentORB_;
                const std::weak_ptr<int> weak = lifetimeToken_;
                (void)scheduler_.ScheduleAfter(
                    kFetchAgentWriteRetryDelayNs, [this, weak, retryORB]() {
                        if (weak.expired()) {
                            return;
                        }
                        if (activeFetchAgentORB_ == retryORB) {
                            (void)AppendImmediate(retryORB);
                        }
                    });
                return;
            }

            SBP2CommandORB* failedORB = activeFetchAgentORB_;
            FailORB(failedORB, -1, Wire::SBPStatus::kUnspecifiedError);
            FailPendingImmediate(-1, Wire::SBPStatus::kUnspecifiedError);
            Clear(true);
            Reset(nullptr);
        }
        return;
    }

    // Write succeeded — the target may now fetch the ORB. Arm its timeout.
    StartORBTimeout(activeFetchAgentORB_);
    activeFetchAgentORB_ = nullptr;

    if (!pendingImmediateORBs_.empty()) {
        SBP2CommandORB* next = pendingImmediateORBs_.front();
        pendingImmediateORBs_.pop_front();
        (void)AppendImmediate(next);
    }
}

// ---------------------------------------------------------------------------
// Status block → ORB matching
// ---------------------------------------------------------------------------

bool FetchAgent::OnStatusBlock(const Wire::StatusBlock& block, uint32_t length) noexcept {
    const uint64_t orbKey = MakeORBKey(OSSwapBigToHostInt16(block.orbOffsetHi), OSSwapBigToHostInt32(block.orbOffsetLo));
    const auto it = outstandingORBs_.find(orbKey);
    if (it == outstandingORBs_.end()) {
        ASFW_LOG(Async, "FetchAgent::OnStatusBlock: unmatched ORB hi=%04x lo=%08x",
                 OSSwapBigToHostInt16(block.orbOffsetHi), OSSwapBigToHostInt32(block.orbOffsetLo));
        return false;
    }

    Outstanding entry = it->second;
    outstandingORBs_.erase(it);
    if (entry.timeoutToken != kInvalidSchedulerToken) {
        scheduler_.Cancel(entry.timeoutToken);
    }

    // Status arrived for this ORB. If its ORB_POINTER write completion has not
    // yet been serviced (activeFetchAgentORB_ still points here — possible when
    // the target ack_pends the write and its status block is processed before
    // our write-response), sever the raw pointer now: the completion callback
    // below runs CommandExecutor::RetireCommand, which frees the ORB, and a late
    // OnFetchAgentWriteComplete must not dereference it. The command is already
    // delivered, so suppressing that path's retry/timeout arming is also correct.
    if (activeFetchAgentORB_ == entry.orb) {
        activeFetchAgentORB_ = nullptr;
    }

    SBP2CommandORB::CompletionCallback cb;
    if (entry.orb != nullptr) {
        entry.orb->SetAppended(false);
        // Hand the command-set-dependent status bytes (8+, SBP-2 Annex B: SCSI
        // status + packed autosense) to the ORB so the executor's completion
        // can surface CHECK CONDITION + sense instead of masking them.
        if (length > 8) {
            const auto* raw = reinterpret_cast<const uint8_t*>(&block);
            const uint32_t dataLen = length - 8 > 24 ? 24 : length - 8;
            entry.orb->SetCompletionStatusData(std::span<const uint8_t>{raw + 8, dataLen});
        }
        cb = entry.orb->GetCompletionCallback();
    }

    // Dead bit set → the target's fetch agent is DEAD and ignores ORB_POINTER
    // writes until reset. Revive it and complete the command only once the
    // reset write has completed: Apple's transport waits for
    // FetchAgentResetComplete before CompleteSCSITask
    // (IOFireWireSerialBusProtocolTransport.cpp:1455) — completing early lets
    // the initiator's next command race the firmware's reset processing.
    // The deferred callback is safe against teardown: CommandExecutor's
    // completion lambda guards on its own lifetime token and active-ORB match.
    if (block.DeadBit() != 0) {
        ASFW_LOG(Async,
                 "FetchAgent::OnStatusBlock: agent DEAD (resp=%u sbp=0x%02x) — reset, "
                 "completion deferred",
                 block.Response(), block.sbpStatus);
        const uint8_t sbpStatus = block.sbpStatus;
        ResetNoWait([cb, sbpStatus]() {
            if (cb) {
                cb(0, sbpStatus);
            }
        });
        return true;
    }

    if (cb) {
        cb(0, block.sbpStatus);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fetch-agent reset
// ---------------------------------------------------------------------------

void FetchAgent::Reset(std::function<void(int)> callback) noexcept {
    if (!bound_ || agentResetInProgress_) {
        if (callback) {
            callback(-1);
        }
        return;
    }

    agentResetInProgress_ = true;
    agentResetCallback_ = std::move(callback);

    const std::weak_ptr<int> weak = lifetimeToken_;
    const uint16_t requestGeneration = binding_.generation;
    agentResetWriteHandle_ = bus_.WriteQuad(
        FW::Generation{binding_.generation},
        FW::NodeId{static_cast<uint8_t>(binding_.nodeID & 0x3Fu)},
        binding_.agentResetAddress,
        0,
        TargetSpeed(),
        [this, weak, requestGeneration](Async::AsyncStatus status, std::span<const uint8_t>) {
            if (weak.expired()) {
                return;
            }
            OnAgentResetComplete(requestGeneration, status);
        });

    if (!agentResetWriteHandle_) {
        ASFW_LOG(Async, "FetchAgent::Reset: WriteQuad failed");
        agentResetInProgress_ = false;
        if (agentResetCallback_) {
            auto cb = std::move(agentResetCallback_);
            agentResetCallback_ = nullptr;
            cb(-1);
        }
    }
}

void FetchAgent::ResetNoWait(std::function<void()> onComplete) noexcept {
    if (!bound_) {
        if (onComplete) {
            onComplete();
        }
        return;
    }
    // Fire-and-forget quadlet to AGENT_RESET — no ORB-tracking teardown, so a
    // command completing right now (and the next one submitted from its
    // completion) is untouched. Mirrors Linux sbp2_agent_reset_no_wait.
    const auto handle = bus_.WriteQuad(
        FW::Generation{binding_.generation},
        FW::NodeId{static_cast<uint8_t>(binding_.nodeID & 0x3Fu)},
        binding_.agentResetAddress,
        0,
        TargetSpeed(),
        [onComplete](Async::AsyncStatus status, std::span<const uint8_t>) {
            ASFW_LOG(Async, "FetchAgent: AGENT_RESET (no-wait) complete status=%d",
                     static_cast<int>(status));
            if (onComplete) {
                onComplete();
            }
        });
    if (!handle) {
        ASFW_LOG(Async, "FetchAgent: AGENT_RESET (no-wait) WriteQuad submit FAILED");
        if (onComplete) {
            onComplete();
        }
    }
}

void FetchAgent::OnAgentResetComplete(uint16_t expectedGeneration,
                                      Async::AsyncStatus status) noexcept {
    if (expectedGeneration != binding_.generation) {
        return;
    }
    agentResetInProgress_ = false;
    Clear(true);

    if (agentResetCallback_) {
        const int result = (status == Async::AsyncStatus::kSuccess) ? 0 : -1;
        auto cb = std::move(agentResetCallback_);
        agentResetCallback_ = nullptr;
        cb(result);
    }
}

// ---------------------------------------------------------------------------
// ORB timeout + failure
// ---------------------------------------------------------------------------

void FetchAgent::StartORBTimeout(SBP2CommandORB* orb) noexcept {
    if (orb == nullptr) {
        return;
    }
    const uint32_t timeoutMs = orb->GetTimeout();
    if (timeoutMs == 0) {
        return;
    }

    const auto it = outstandingORBs_.find(MakeORBKey(orb->GetORBAddress()));
    if (it == outstandingORBs_.end()) {
        return;
    }
    if (it->second.timeoutToken != kInvalidSchedulerToken) {
        scheduler_.Cancel(it->second.timeoutToken);
    }

    const std::weak_ptr<int> weak = lifetimeToken_;
    const uint64_t key = it->first;
    it->second.timeoutToken = scheduler_.ScheduleAfter(
        static_cast<uint64_t>(timeoutMs) * 1'000'000ULL, [this, weak, key]() {
            if (weak.expired()) {
                return;
            }
            const auto entryIt = outstandingORBs_.find(key);
            if (entryIt == outstandingORBs_.end()) {
                return;
            }
            SBP2CommandORB* timedOut = entryIt->second.orb;
            // The agent is likely wedged mid-fetch (or dead) — revive it so the
            // NEXT command's ORB_POINTER is honored. Linux aborts timed-out
            // commands the same way (sbp2_scsi_abort → agent reset).
            ASFW_LOG(Async, "FetchAgent: ORB timeout — agent reset before failing");
            ResetNoWait();
            FailORB(timedOut, -1, Wire::SBPStatus::kUnspecifiedError);
        });
}

void FetchAgent::FailORB(SBP2CommandORB* orb, int transportStatus, uint8_t sbpStatus) noexcept {
    if (orb == nullptr) {
        return;
    }
    const auto it = outstandingORBs_.find(MakeORBKey(orb->GetORBAddress()));
    if (it != outstandingORBs_.end()) {
        if (it->second.timeoutToken != kInvalidSchedulerToken) {
            scheduler_.Cancel(it->second.timeoutToken);
        }
        outstandingORBs_.erase(it);
    }
    pendingImmediateORBs_.erase(
        std::remove(pendingImmediateORBs_.begin(), pendingImmediateORBs_.end(), orb),
        pendingImmediateORBs_.end());
    if (activeFetchAgentORB_ == orb) {
        activeFetchAgentORB_ = nullptr;
    }
    orb->SetAppended(false);

    auto cb = orb->GetCompletionCallback();
    if (cb) {
        cb(transportStatus, sbpStatus);
    }
}

void FetchAgent::FailPendingImmediate(int transportStatus, uint8_t sbpStatus) noexcept {
    auto pending = pendingImmediateORBs_;
    for (auto* orb : pending) {
        FailORB(orb, transportStatus, sbpStatus);
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint16_t FetchAgent::LocalBusNodeID() const noexcept {
    return NormalizeBusNodeID(static_cast<uint16_t>(busInfo_.GetLocalNodeID().value));
}

FW::FwSpeed FetchAgent::TargetSpeed() const noexcept {
    return busInfo_.GetSpeed(FW::NodeId{static_cast<uint8_t>(binding_.nodeID & 0x3Fu)});
}

uint64_t FetchAgent::MakeORBKey(uint16_t addressHi, uint32_t addressLo) noexcept {
    return (static_cast<uint64_t>(addressHi) << 32) | static_cast<uint64_t>(addressLo);
}

uint64_t FetchAgent::MakeORBKey(const Async::FWAddress& address) noexcept {
    return MakeORBKey(address.addressHi, address.addressLo);
}

uint16_t FetchAgent::MaxPayloadLog() const noexcept {
    uint16_t payloadBytes = binding_.maxPayloadSize;
    if (payloadBytes > 4096) {
        payloadBytes = 4096;
    }
    const uint16_t quadlets = payloadBytes / 4;
    if (quadlets == 0) {
        return 0;
    }
    uint16_t log = 0;
    while ((1u << log) < quadlets && log < 15) {
        ++log;
    }
    return log;
}

} // namespace ASFW::Protocols::SBP2
