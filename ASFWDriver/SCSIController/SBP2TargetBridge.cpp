// SBP2TargetBridge — HBA ↔ SBP-2 session glue. See SBP2TargetBridge.hpp.

#include "SBP2TargetBridge.hpp"

#include "SBP2BridgeHub.hpp"
#include "../Discovery/FWDevice.hpp"
#include "../Discovery/FWUnit.hpp"
#include "../Logging/Logging.hpp"

#include <utility>

namespace ASFW::Protocols::SBP2 {

namespace {

class IOLockGuard {
public:
    explicit IOLockGuard(IOLock* lock) : lock_(lock) {
        if (lock_ != nullptr) {
            IOLockLock(lock_);
        }
    }
    ~IOLockGuard() {
        if (lock_ != nullptr) {
            IOLockUnlock(lock_);
        }
    }
    IOLockGuard(const IOLockGuard&) = delete;
    IOLockGuard& operator=(const IOLockGuard&) = delete;

private:
    IOLock* lock_{nullptr};
};

} // namespace

SBP2TargetBridge::SBP2TargetBridge(SessionRegistry& registry,
                                   Discovery::IDeviceManager& deviceManager,
                                   IODispatchQueue* workQueue)
    : registry_(registry)
    , deviceManager_(deviceManager)
    , workQueue_(workQueue) {
    lock_ = IOLockAlloc();
}

SBP2TargetBridge::~SBP2TargetBridge() {
    // Shutdown() must already have run (driver Stop path); this is a backstop.
    Shutdown();
    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void SBP2TargetBridge::Start() {
    std::weak_ptr<SBP2TargetBridge> weak = weak_from_this();
    IODispatchQueue* queue = workQueue_;
    unitCallbackHandle_ = deviceManager_.RegisterUnitCallback(
        kSBP2UnitSpecId, kSBP2UnitSwVersion,
        [weak, queue](std::shared_ptr<Discovery::FWUnit> unit) {
            // DeviceManager fires this callback while holding its own lock;
            // OnUnitPublished → CreateSession → ResolveUnit → GetAllDevices()
            // re-enters that lock (os_unfair_lock recursion = abort). Defer one
            // queue iteration so the lock is released first.
            if (queue == nullptr) {
                return;
            }
            queue->DispatchAsync(^{
                if (auto self = weak.lock()) {
                    self->OnUnitPublished(unit);
                }
            });
        });
    unitCallbackRegistered_ = true;

    // Push channel: the registry fires this (on our work queue) when a session
    // logs in/out. Forward to the HBA via the hub. Registered before adopting
    // existing units so a login that completes during adoption is not missed.
    registry_.SetLoginStateObserver([weak](uint64_t guid, bool loggedIn) {
        if (auto self = weak.lock()) {
            self->OnLoginStateChanged(guid, loggedIn);
        }
    });

    AdoptExistingUnits();
    ASFW_LOG(Controller, "[SBP2Bridge] started (watching for SBP-2 units)");
}

void SBP2TargetBridge::Shutdown() {
    uint64_t handle = 0;
    std::deque<PendingTask> drained;
    {
        IOLockGuard g(lock_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        handle = sessionHandle_;
        sessionHandle_ = 0;
        drained.swap(pending_);
    }

    if (unitCallbackRegistered_) {
        deviceManager_.UnregisterCallback(unitCallbackHandle_);
        unitCallbackRegistered_ = false;
        registry_.SetLoginStateObserver(nullptr);
    }

    // Releases the session; an in-flight command is aborted through our
    // completion callback (which sees stopping_ and does not re-pump).
    if (handle != 0) {
        registry_.ReleaseOwner(this);
    }

    for (auto& task : drained) {
        task.callback(SyntheticFailure(static_cast<int>(kIOReturnAborted)));
    }
    if (!drained.empty()) {
        ASFW_LOG(Controller, "[SBP2Bridge] shutdown drained %zu queued tasks", drained.size());
    }
}

bool SBP2TargetBridge::IsReady() const {
    uint64_t handle = 0;
    {
        IOLockGuard g(lock_);
        if (stopping_) {
            return false;
        }
        handle = sessionHandle_;
    }
    if (handle == 0) {
        return false;
    }
    auto state = registry_.GetSessionState(const_cast<SBP2TargetBridge*>(this), handle);
    return state.has_value() && state->loginState == LoginState::LoggedIn;
}

void SBP2TargetBridge::SubmitTask(SCSI::CommandRequest request, TaskCallback callback) {
    if (!callback) {
        return;
    }
    {
        IOLockGuard g(lock_);
        if (!stopping_) {
            pending_.push_back(PendingTask{std::move(request), std::move(callback)});
            SchedulePump();
            return;
        }
    }
    callback(SyntheticFailure(static_cast<int>(kIOReturnAborted)));
}

void SBP2TargetBridge::OnUnitPublished(const std::shared_ptr<Discovery::FWUnit>& unit) {
    if (!unit) {
        return;
    }
    auto device = unit->GetDevice();
    if (!device) {
        return;
    }
    const uint64_t guid = device->GetGUID();
    const uint32_t romOffset = unit->GetDirectoryOffset();

    uint64_t existing = 0;
    {
        IOLockGuard g(lock_);
        if (stopping_) {
            return;
        }
        existing = sessionHandle_;
    }

    if (existing != 0) {
        const auto state = registry_.GetSessionState(this, existing);
        if (state.has_value()) {
            switch (state->loginState) {
            case LoginState::Idle:
                // Session exists but never logged in — kick it.
                (void)registry_.StartLogin(this, existing);
                return;
            case LoginState::Failed:
                // Dead session (login retries exhausted / failed reconnect) —
                // release and build a fresh one below.
                (void)registry_.ReleaseSession(this, existing);
                {
                    IOLockGuard g(lock_);
                    sessionHandle_ = 0;
                }
                break;
            default:
                // LoggingIn/LoggedIn/Suspended/Reconnecting: healthy or being
                // handled by RefreshTargets — leave it alone.
                return;
            }
        } else {
            IOLockGuard g(lock_);
            sessionHandle_ = 0;
        }
    }

    auto handle = registry_.CreateSession(this, guid, romOffset);
    if (!handle.has_value()) {
        // kIOReturnExclusiveAccess: someone else (e.g. the probe tool) owns this
        // target — stay out of the way.
        ASFW_LOG(Controller,
                 "[SBP2Bridge] CreateSession failed 0x%x (guid=0x%016llx romOffset=%u)",
                 handle.error(), guid, romOffset);
        return;
    }
    {
        IOLockGuard g(lock_);
        if (stopping_) {
            return;
        }
        sessionHandle_ = *handle;
    }
    ASFW_LOG(Controller, "[SBP2Bridge] session %llu created for guid=0x%016llx — logging in",
             *handle, guid);
    if (!registry_.StartLogin(this, *handle)) {
        ASFW_LOG(Controller, "[SBP2Bridge] StartLogin failed for session %llu", *handle);
    }
}

void SBP2TargetBridge::OnLoginStateChanged(uint64_t guid, bool loggedIn) {
    // Phase 1: the guid->targetID map (WI-3) and the actual
    // UserCreateTargetForID/UserDestroyTargetForID (WI-4) are not wired yet.
    // Forward the raw event to the HBA, which logs it inertly — the static
    // phantom target still serves SCSI probes unchanged.
    ASFW_LOG(Controller, "[SBP2Bridge] login %s guid=0x%016llx",
             loggedIn ? "up" : "down", guid);
    SBP2BridgeHub::NotifyTargetState(guid, loggedIn);
}

void SBP2TargetBridge::AdoptExistingUnits() {
    const auto units = deviceManager_.FindUnitsBySpec(kSBP2UnitSpecId, kSBP2UnitSwVersion);
    for (const auto& unit : units) {
        OnUnitPublished(unit);
    }
}

void SBP2TargetBridge::SchedulePump() {
    // Caller holds lock_.
    if (stopping_ || pumpScheduled_ || workQueue_ == nullptr) {
        return;
    }
    pumpScheduled_ = true;
    std::weak_ptr<SBP2TargetBridge> weak = weak_from_this();
    workQueue_->DispatchAsync(^{
        if (auto self = weak.lock()) {
            self->Pump();
        }
    });
}

void SBP2TargetBridge::Pump() {
    for (;;) {
        PendingTask task;
        {
            IOLockGuard g(lock_);
            pumpScheduled_ = false;
            if (stopping_ || commandInFlight_ || pending_.empty()) {
                return;
            }
            task = std::move(pending_.front());
            pending_.pop_front();
            commandInFlight_ = true;
        }

        uint64_t handle = 0;
        {
            IOLockGuard g(lock_);
            handle = sessionHandle_;
        }

        bool submitted = false;
        if (handle != 0) {
            // The completion callback runs on the Default queue (ORB completion)
            // or under the registry lock (teardown abort) — never re-enter the
            // registry from it synchronously; re-pump via DispatchAsync.
            std::weak_ptr<SBP2TargetBridge> weak = weak_from_this();
            TaskCallback taskCallback = task.callback;
            submitted = registry_.SubmitCommand(
                this, handle, task.request,
                [weak, taskCallback](const SCSI::CommandResult& result) {
                    auto self = weak.lock();
                    if (self) {
                        IOLockGuard g(self->lock_);
                        self->commandInFlight_ = false;
                    }
                    taskCallback(result);
                    if (self) {
                        IOLockGuard g(self->lock_);
                        if (!self->stopping_ && !self->pending_.empty()) {
                            self->SchedulePump();
                        }
                    }
                });
        }

        if (submitted) {
            return;
        }

        // No session / not logged in / executor busy — fail this task and move on.
        {
            IOLockGuard g(lock_);
            commandInFlight_ = false;
        }
        task.callback(SyntheticFailure(static_cast<int>(kIOReturnNotReady)));
    }
}

SCSI::CommandResult SBP2TargetBridge::SyntheticFailure(int transportStatus) {
    SCSI::CommandResult result{};
    result.transportStatus = transportStatus;
    return result;
}

} // namespace ASFW::Protocols::SBP2
