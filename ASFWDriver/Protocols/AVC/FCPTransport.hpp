//
// FCPTransport.hpp
// ASFWDriver - AV/C Protocol Layer
//
// The AV/C transaction engine for one unit: FCP command/response exchange over
// IEEE 1394 block writes (IEC 61883-1 §9, TA 2004006 AV/C General 4.2 §6).
//
// One transaction is active at a time; later submissions wait in FIFO order.
// The active transaction moves through explicit phases (see Phase below),
// following Apple's command states (IOFireWireAVCCommand.cpp:66-74) and Linux
// fcp_avc_transaction (sound/firewire/fcp.c:231-286):
//
//   Writing          the command block write is in flight
//   AwaitingResponse the write reached the target; waiting for the response
//                    (an INTERIM response extends the deadline)
//   AwaitingRoute    a bus reset interrupted a STATUS/INQUIRY that may be
//                    replayed once discovery rebinds the unit
//   Answered         the final response was accepted; it is delivered from the
//                    work queue, after our write response to it (§6.5)
//
// Callers submit through IAvcUnit::Submit only. Every failure is an AvcError;
// ToIOReturn(AvcError) is the one mapping to IOReturn.
//

#pragma once

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/IODispatchQueue.h>
#endif
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include "AVCDefs.hpp"
#include "AVCCommandFilter.hpp"
#include "FcpExchangeRecorder.hpp"
#include "Core/IAvcUnit.hpp"
#include "../Ports/FireWireBusPort.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

namespace ASFW::Protocols::AVC {

//==============================================================================
// FCP Frame
//==============================================================================

/// An FCP frame (command or response payload) as it travels on the bus.
struct FCPFrame {
    std::array<uint8_t, kAVCFrameMaxSize> data{};
    size_t length{0};

    std::span<const uint8_t> Payload() const { return {data.data(), length}; }
    std::span<uint8_t> MutablePayload() { return {data.data(), length}; }

    bool IsValid() const {
        return length >= kAVCFrameMinSize && length <= kAVCFrameMaxSize;
    }
};

//==============================================================================
// Configuration
//==============================================================================

struct FCPTransportConfig {
    /// FCP command CSR address (the target receives commands here).
    uint64_t commandAddress{kFCPCommandAddress};

    /// FCP response CSR address (we receive responses here).
    uint64_t responseAddress{kFCPResponseAddress};

    /// Response deadline after the command write completes (milliseconds).
    uint32_t timeoutMs{kFCPTimeoutInitial};

    /// Response deadline after an INTERIM response (milliseconds).
    uint32_t interimTimeoutMs{kFCPTimeoutAfterInterim};

    /// Replays of a STATUS/INQUIRY after a lost response or failed write.
    /// CONTROL and NOTIFY are never replayed.
    uint8_t maxRetries{kFCPMaxRetries};

    /// Let a STATUS/INQUIRY interrupted by a bus reset replay once discovery
    /// rebinds the unit (default: fail it with kBusReset).
    bool allowBusResetRetry{false};

    /// Allowlist of command shapes this device may be sent. Empty (the default)
    /// means unrestricted, which is what every ordinary device carries. When
    /// non-empty, Submit refuses any frame that matches no entry: the guard for
    /// firmware that hangs on unimplemented AV/C. The span is non-owning and
    /// must outlive the transport; all tables in AVCCommandFilter.hpp are
    /// constexpr statics.
    std::span<const FCPPermittedFrame> permittedFrames;
};

//==============================================================================
// Transaction engine
//==============================================================================

class FCPTransport final : public std::enable_shared_from_this<FCPTransport>,
                           public ASFW::AVC::IAvcUnit {
public:
    FCPTransport() = default;
    ~FCPTransport() override;

    FCPTransport(const FCPTransport&) = delete("one transport per unit owns the single outstanding FCP transaction");
    FCPTransport& operator=(const FCPTransport&) = delete("one transport per unit owns the single outstanding FCP transaction");

    bool init(Protocols::Ports::FireWireBusOps* busOps,
              Protocols::Ports::FireWireBusInfo* busInfo,
              Discovery::FWDevice* device,
              Discovery::DeviceRegistry& routeRegistry,
              Scheduling::ITimerScheduler& timerScheduler,
              const FCPTransportConfig& config = {});

    // --- IAvcUnit ---
    /// Queue `frame` for this unit. STATUS and INQUIRY may be replayed on a lost
    /// response; CONTROL and NOTIFY never are. The command is refused with
    /// kBusReset if the unit's route is no longer in `generation`.
    void Submit(const ASFW::AVC::CommandFrame& frame,
                FW::Generation generation,
                ResponseCallback completion) override;

    [[nodiscard]] FW::NodeId NodeId() const noexcept override;
    [[nodiscard]] FW::Generation CurrentGeneration() const noexcept override;
    [[nodiscard]] uint64_t Guid() const noexcept override;

    // --- Events from the bus and discovery ---
    /// The target wrote a response frame to our FCP_RESPONSE register. Runs
    /// inside the receive handler.
    void OnFCPResponse(uint16_t srcNodeID,
                       uint32_t generation,
                       std::span<const uint8_t> payload);

    void OnBusReset(uint32_t newGeneration);

    /// Resume an interrupted STATUS/INQUIRY only after discovery has rebound
    /// this unit to the reset generation. A no-op unless one is waiting.
    void OnRouteRevalidated(const Discovery::DeviceRouteToken& route);

    /// Complete every transaction with kTransportError; no more bus I/O.
    void Shutdown();

    const FCPTransportConfig& GetConfig() const { return config_; }

    /// Start a new exchange log (attach, manual refresh).
    void BeginExchangeSession();
    /// A copy of every exchange since the session started.
    [[nodiscard]] FcpExchangeLog CopyExchangeLog() const;

private:
    using Result = ASFW::AVC::Expected<FCPFrame>;

    /// One block write of a command. A response matches only after this exact
    /// attempt was issued on this route. Linux pairs destination identity with
    /// generation at request issue (core-transaction.c:285-303, 363-372); Apple
    /// keeps fWriteNodeID/fWriteGen on its write command
    /// (IOFireWireAVCCommand.cpp:481-491).
    struct WriteAttempt {
        uint64_t id{0};
        Discovery::DeviceRouteToken route{};
    };

    struct Transaction {
        uint32_t id{0};
        ASFW::AVC::CommandFrame frame;
        ResponseCallback completion;
        /// STATUS/INQUIRY: safe to replay. CONTROL/NOTIFY: never replayed.
        bool idempotent{false};
        /// The generation the command was built for. A bus-reset replay moves
        /// it to the rebound route's generation.
        FW::Generation generation{0U};
        uint8_t retriesLeft{0};
        bool sawInterim{false};
    };

    // Phases of the active transaction.
    struct Writing {
        WriteAttempt attempt;
        Async::AsyncHandle handle{};
    };
    struct AwaitingResponse {
        WriteAttempt attempt;
    };
    struct AwaitingRoute {
        std::optional<Discovery::DeviceRouteToken> resetRoute;
    };
    struct Answered {};
    using Phase = std::variant<Writing, AwaitingResponse, AwaitingRoute, Answered>;

    struct Active {
        Transaction txn;
        Phase phase;
        Scheduling::TimerToken timer{Scheduling::kInvalidTimerToken};
        uint64_t timerEpoch{0};
    };

    struct Delivery {
        Transaction txn;
        Result result;
    };

    // Event handlers take lock_ themselves and are called without it. The
    // helpers marked "lock held" must be called with it.

    /// Make `txn` active and issue its first write.
    void Start(Transaction txn);
    /// Issue the active transaction's write on the current route.
    void IssueWrite();
    /// The active transaction's write finished.
    void OnWriteComplete(WriteAttempt attempt, Async::AsyncStatus status);
    void OnTimeout(uint64_t epoch);
    /// Replay the active transaction (idempotent only).
    void Replay();
    /// Start the next queued transaction if none is active.
    void StartNext();

    [[nodiscard]] static std::optional<WriteAttempt> AttemptOf(const Phase& phase) noexcept;
    [[nodiscard]] static bool ResponseMatches(const Transaction& txn, std::span<const uint8_t> response);

    void ArmTimer(uint32_t timeoutMs);  // lock held
    void DisarmTimer();                 // lock held

    /// Take the active transaction out with `result` for delivery. Lock held.
    [[nodiscard]] Delivery Finish(Result result);
    /// Finish the active transaction if it is still `id`.
    void FinishIfActive(uint32_t id, Result result);
    /// Record an exchange in the log. Lock held.
    void Record(const Transaction& txn, const Result& result);

    /// Run deliveries in a loop at constant stack depth, starting the next
    /// queued transaction after each. A completion may submit again, and a
    /// submission may fail at once; nested, a 227-command attach overflowed
    /// the stack.
    void Deliver(Delivery delivery);
    /// Parse and hand one result to its caller.
    static void Invoke(Delivery& delivery);

    Protocols::Ports::FireWireBusOps* busOps_{nullptr};
    Protocols::Ports::FireWireBusInfo* busInfo_{nullptr};
    Discovery::FWDevice* device_{nullptr};
    Discovery::DeviceRegistry* routeRegistry_{nullptr};
    Scheduling::ITimerScheduler* timerScheduler_{nullptr};
    FCPTransportConfig config_;

    IOLock* lock_{nullptr};
    bool shuttingDown_{false};
    uint32_t nextTransactionID_{0};
    uint64_t nextWriteAttempt_{0};
    uint64_t nextTimerEpoch_{0};

    std::optional<Active> active_;
    std::deque<Transaction> queue_;
    FcpExchangeRecorder recorder_;

    std::deque<Delivery> deliveries_;
    bool delivering_{false};
};

} // namespace ASFW::Protocols::AVC
