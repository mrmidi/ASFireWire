#pragma once

// FetchAgent — SBP-2 command-plane engine.
//
// Submits Normal Command ORBs to a logged-in target: writes the first immediate
// ORB to the fetch-agent register, chains subsequent ORBs via their next-ORB
// pointer, rings the doorbell, retries failed fetch-agent writes, tracks
// outstanding ORBs, times them out, and matches incoming status blocks back to
// the ORB that produced them.
//
// Owned by SBP2LoginSession (composition). The login *state* (LoginState,
// loginID, generation) stays in LoginSession; FetchAgent holds only the ORB
// mechanics and is driven through an explicit Binding the session supplies once
// a login or reconnect succeeds. CommandExecutor submits ORBs via the session.
//
// Timers go through the injected ISessionScheduler (never the IOSleep-on-queue
// path). Async bus callbacks are guarded by a weak lifetime token.

#include "ISessionScheduler.hpp"
#include "../SBP2CommandORB.hpp"
#include "../SBP2WireFormats.hpp"
#include "../../../Async/AsyncTypes.hpp"
#include "../../../Async/Interfaces/IFireWireBus.hpp"
#include "../../../Logging/Logging.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>

namespace ASFW::Protocols::SBP2 {

class FetchAgent {
public:
    // Supplied by LoginSession when a login/reconnect succeeds. Holds the target
    // node + generation and the three command-block-agent register addresses.
    struct Binding {
        uint16_t generation{0};
        uint16_t nodeID{0xFFFF};
        Async::FWAddress fetchAgentAddress{};
        Async::FWAddress doorbellAddress{};
        Async::FWAddress agentResetAddress{};
        uint16_t maxPayloadSize{4096};
    };

    FetchAgent(Async::IFireWireBus& bus,
               Async::IFireWireBusInfo& busInfo,
               ISessionScheduler& scheduler) noexcept;
    ~FetchAgent();

    FetchAgent(const FetchAgent&) = delete;
    FetchAgent& operator=(const FetchAgent&) = delete;

    // Bind to a freshly logged-in target. Re-binding (reconnect) updates the
    // generation/addresses; outstanding ORBs are cleared first.
    void Bind(const Binding& binding) noexcept;

    // Detach (logout / lost). Cancels and drops all outstanding ORBs.
    void Unbind() noexcept;

    [[nodiscard]] bool IsBound() const noexcept { return bound_; }

    // Submit a Normal Command ORB. Returns false if not bound, or the ORB is
    // null / not allocated (IsValid) / already appended.
    [[nodiscard]] bool Submit(SBP2CommandORB* orb) noexcept;

    // Feed a status block arriving on the session's status FIFO. Returns true if
    // it matched and completed an outstanding ORB; false if unsolicited.
    [[nodiscard]] bool OnStatusBlock(const Wire::StatusBlock& block,
                                     uint32_t length) noexcept;

    // Issue a fetch-agent reset. callback(0)=success, callback(-1)=failure. The
    // ORB chain is cleared once the reset completes.
    void Reset(std::function<void(int)> callback) noexcept;

    // Fire-and-forget AGENT_RESET write (Linux sbp2_agent_reset_no_wait).
    // Unlike Reset(), does NOT clear ORB tracking on completion — used to
    // revive a dead/wedged agent inline (dead bit in a status block, ORB
    // timeout) without disturbing the command that is being completed.
    void ResetNoWait() noexcept;

    // Cancel timers and drop all ORB tracking (bus reset / logout).
    void Clear(bool cancelTimers) noexcept;

    // Test seam: override the per-ORB fetch-agent-write retry budget.
    void SetWriteRetriesForTesting(uint32_t retries) noexcept { defaultWriteRetries_ = retries; }

private:
    struct Outstanding {
        SBP2CommandORB* orb{nullptr};
        SchedulerToken timeoutToken{kInvalidSchedulerToken};
    };

    [[nodiscard]] bool AppendImmediate(SBP2CommandORB* orb) noexcept;
    [[nodiscard]] bool AppendChained(SBP2CommandORB* orb) noexcept;
    void RingDoorbell() noexcept;

    void OnFetchAgentWriteComplete(uint16_t expectedGeneration,
                                   Async::AsyncStatus status) noexcept;
    void OnDoorbellComplete(uint16_t expectedGeneration,
                            Async::AsyncStatus status) noexcept;
    void OnAgentResetComplete(uint16_t expectedGeneration,
                              Async::AsyncStatus status) noexcept;

    void StartORBTimeout(SBP2CommandORB* orb) noexcept;
    void FailORB(SBP2CommandORB* orb, int transportStatus, uint8_t sbpStatus) noexcept;
    void FailPendingImmediate(int transportStatus, uint8_t sbpStatus) noexcept;
    void CompleteORB(SBP2CommandORB* orb, int transportStatus, uint8_t sbpStatus) noexcept;

    [[nodiscard]] uint16_t LocalBusNodeID() const noexcept;
    [[nodiscard]] FW::FwSpeed TargetSpeed() const noexcept;
    [[nodiscard]] static uint64_t MakeORBKey(uint16_t addressHi, uint32_t addressLo) noexcept;
    [[nodiscard]] static uint64_t MakeORBKey(const Async::FWAddress& address) noexcept;
    [[nodiscard]] uint16_t MaxPayloadLog() const noexcept;

    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    ISessionScheduler& scheduler_;

    bool bound_{false};
    Binding binding_{};

    std::unordered_map<uint64_t, Outstanding> outstandingORBs_;
    std::deque<SBP2CommandORB*> pendingImmediateORBs_;
    SBP2CommandORB* chainTailORB_{nullptr};
    SBP2CommandORB* activeFetchAgentORB_{nullptr};

    std::array<uint8_t, 8> fetchAgentWriteData_{};
    Async::AsyncHandle fetchAgentWriteHandle_{};
    bool fetchAgentWriteInUse_{false};

    Async::AsyncHandle doorbellWriteHandle_{};
    bool doorbellInProgress_{false};
    bool doorbellRingAgain_{false};

    Async::AsyncHandle agentResetWriteHandle_{};
    bool agentResetInProgress_{false};
    std::function<void(int)> agentResetCallback_;

    uint32_t defaultWriteRetries_{20};
    std::shared_ptr<int> lifetimeToken_{std::make_shared<int>(0)};
};

} // namespace ASFW::Protocols::SBP2
