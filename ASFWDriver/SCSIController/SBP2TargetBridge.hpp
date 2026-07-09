#pragma once

// SBP2TargetBridge — Phase 1 glue between the synthetic SCSI HBA
// (ASFWSCSIController) and the SBP-2 session/command layer.
//
// Owns the scanner-target lifecycle on the driver side (no userspace involved):
//   * watches discovery for an SBP-2 unit (spec 0x00609E / sw 0x010483),
//   * creates a session in the SessionRegistry (owner = this) and starts login,
//   * accepts SCSI tasks from the HBA on any queue, serializes them FIFO
//     (SBP-2 command plane is one-in-flight), and pushes each through
//     SessionRegistry::SubmitCommand with a completion callback.
//
// Threading: discovery callbacks, pump iterations, and command completions all
// run on the driver's Default work queue. SubmitTask/IsReady may be called from
// the HBA service's queues. lock_ guards the pending queue + flags; it is NEVER
// held across a registry call (the registry has its own lock, and executor
// teardown can fire our completion callback while holding it — holding lock_
// into a registry call would be an ABBA deadlock).
//
// Lifetime: create via std::make_shared. Scheduled pump blocks capture
// weak_from_this() so blocks queued behind teardown fizzle instead of touching
// a dead bridge. Shutdown() (driver Stop path) stops intake, releases the
// session (which aborts any in-flight command back through our callback), and
// drains queued tasks with an aborted result.

#include "../Protocols/SBP2/Session/SessionRegistry.hpp"
#include "../Discovery/IDeviceManager.hpp"

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#endif

#include <deque>
#include <functional>
#include <memory>

namespace ASFW::Protocols::SBP2 {

class SBP2TargetBridge : public std::enable_shared_from_this<SBP2TargetBridge> {
public:
    using TaskCallback = std::function<void(const SCSI::CommandResult&)>;

    SBP2TargetBridge(const std::shared_ptr<SessionRegistry>& registry,
                     Discovery::IDeviceManager& deviceManager,
                     IODispatchQueue* workQueue);
    ~SBP2TargetBridge();

    SBP2TargetBridge(const SBP2TargetBridge&) = delete;
    SBP2TargetBridge& operator=(const SBP2TargetBridge&) = delete;

    // Register the discovery callback + adopt any already-published SBP-2 unit.
    // Call once after construction (needs shared_from_this, so not in the ctor).
    void Start();

    // Stop intake, release the session (aborting the in-flight command), drain
    // queued tasks with aborted results. Runs on the Default queue (driver Stop).
    void Shutdown();

    // True when the SBP-2 session is logged in (commands can flow).
    [[nodiscard]] bool IsReady() const;

    // Queue one SCSI task. The callback fires exactly once, on the driver's
    // Default queue, with the command result (synthetic aborted/not-ready result
    // if the task never reached the wire). Callable from any queue.
    void SubmitTask(SCSI::CommandRequest request, TaskCallback callback);

private:
    struct PendingTask {
        SCSI::CommandRequest request;
        TaskCallback callback;
    };

    // Default-queue context only.
    void OnUnitPublished(const std::shared_ptr<Discovery::FWUnit>& unit);
    void OnLoginStateChanged(uint64_t guid, bool loggedIn);
    void AdoptExistingUnits();
    void Pump();

    void SchedulePump();
    [[nodiscard]] static SCSI::CommandResult SyntheticFailure(int transportStatus);

    // Weak, not a bare reference: the HBA reaches IsReady()/SubmitTask() from a
    // separate IOService on a separate queue holding only a shared_ptr to THIS
    // bridge (via SBP2BridgeHub), which does not keep the registry alive. The
    // registry can be freed by ServiceContext::Reset() on the driver's teardown
    // queue concurrently. lock() before every deref: a live strong ref keeps the
    // registry alive for that call, or a null lock is treated as not-ready. This
    // does not extend the registry's lifetime (in-flight commands are still
    // force-completed by the registry's own Cleanup on teardown).
    std::weak_ptr<SessionRegistry> registry_;
    Discovery::IDeviceManager& deviceManager_;
    IODispatchQueue* workQueue_{nullptr};

    IOLock* lock_{nullptr};
    std::deque<PendingTask> pending_;
    bool commandInFlight_{false};
    bool pumpScheduled_{false};
    bool stopping_{false};
    uint64_t sessionHandle_{0};

    Discovery::IUnitRegistry::CallbackHandle unitCallbackHandle_{0};
    bool unitCallbackRegistered_{false};
};

} // namespace ASFW::Protocols::SBP2
