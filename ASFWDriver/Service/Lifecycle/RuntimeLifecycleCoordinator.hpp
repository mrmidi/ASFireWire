#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include "../../Controller/ControllerStateMachine.hpp"

struct IOLock;

namespace ASFW::Driver {

enum class QuiesceReason : uint8_t {
    kPlannedStop,
    kSystemSuspend,
    kProviderRevoked,
    kStartFailure,
    kWakeRebuild,
};

// A private resource-unwind ledger. This is deliberately not a lifecycle
// state: it records only which start stages may need cleanup after failure.
enum class StartStage : uint8_t {
    kNone,
    kDependenciesReady,
    kQueueReady,
    kProviderOpened,
    kInterruptSourceReady,
    kAsyncReady,
    kControllerReady,
    kRunning,
};

struct QuiescePlan {
    QuiesceReason reason{QuiesceReason::kPlannedStop};
    ControllerState stateBefore{ControllerState::kStopped};
    StartStage completedStartStage{StartStage::kNone};
    bool runTeardown{false};
    bool revokeImmediately{false};
};

// Sole authority for root runtime transitions and work admission. Resource
// teardown remains in the service-specific executor, but it runs only after a
// plan issued here and reports completion back here.
class RuntimeLifecycleCoordinator final {
public:
    explicit RuntimeLifecycleCoordinator(std::shared_ptr<ControllerStateMachine> stateMachine);
    ~RuntimeLifecycleCoordinator();

    [[nodiscard]] bool BeginStart(std::string_view reason, uint64_t now) noexcept;
    void MarkStageComplete(StartStage stage) noexcept;
    [[nodiscard]] bool CompleteStart(std::string_view reason, uint64_t now) noexcept;

    [[nodiscard]] std::optional<QuiescePlan>
    BeginFailedStart(std::string_view reason, uint64_t now) noexcept;
    [[nodiscard]] std::optional<QuiescePlan>
    BeginQuiesce(QuiesceReason reason, std::string_view detail, uint64_t now) noexcept;
    void CompleteQuiesce(const QuiescePlan& plan, std::string_view detail, uint64_t now) noexcept;

    [[nodiscard]] bool AdmitsNormalWork() const noexcept;
    [[nodiscard]] bool AdmitsBringupInterrupts() const noexcept;
    [[nodiscard]] ControllerState CurrentState() const noexcept;
    [[nodiscard]] StartStage CompletedStartStage() const noexcept;

private:
    [[nodiscard]] static bool IsSuspendReason(QuiesceReason reason) noexcept;
    [[nodiscard]] static bool IsProviderRevocation(QuiesceReason reason) noexcept;

    std::shared_ptr<ControllerStateMachine> stateMachine_;
    IOLock* lock_{nullptr};
    StartStage completedStartStage_{StartStage::kNone};
    bool teardownActive_{false};
};

} // namespace ASFW::Driver
