// SPDX-License-Identifier: Apache-2.0
// Adapted from the ASFW midi configuration reducer; current-main ownership.
#pragma once

#include "DeviceConfiguration.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>
#include <variant>

namespace ASFW::Configuration {

using EndpointId = uint64_t;
using TransactionToken = uint64_t;
using RouteGeneration = uint64_t;
using ConfigurationRevision = uint64_t;

struct ConfigurationIdentity final {
    EndpointId endpointId{0};
    TransactionToken token{0};
    RouteGeneration routeGeneration{0};
};

struct CommittedConfiguration final {
    EndpointId endpointId{0};
    RouteGeneration routeGeneration{0};
    ConfigurationRevision revision{0};
    DeviceConfiguration configuration{};
};

enum class ConfigurationOrigin : uint8_t {
    ControlClient,
    CoreAudio,
    HardwareObservation,
    Recovery,
};

enum class FailureReason : uint8_t {
    CandidateRejected,
    ADKWindowRejected,
    HardwareRejected,
    ADKAborted,
    ProjectionFailed,
    RouteInvalidated,
    HardwareStateUnknown,
};

struct ConfigurationFailure final {
    FailureReason reason{FailureReason::CandidateRejected};
};

struct TransitionContext final {
    ConfigurationIdentity identity{};
    ConfigurationOrigin origin{ConfigurationOrigin::ControlClient};
    DeviceConfiguration requested{};
    // The accepted candidate includes its immutable resolved wire/HAL snapshot.
    DeviceConfiguration candidate{};
    CommittedConfiguration prior{};
};

struct ConfirmedHardwareConfiguration final {
    DeviceConfiguration configuration{};
};

struct ProjectionPlan final {
    ConfigurationIdentity identity{};
    CommittedConfiguration prior{};
    ConfirmedHardwareConfiguration confirmed{};
};

struct ApplyCandidate final {};
struct ProjectObserved final {
    ConfirmedHardwareConfiguration confirmed{};
};
using PendingWork = std::variant<ApplyCandidate, ProjectObserved>;

struct Uninitialized final {};
struct Idle final {
    CommittedConfiguration committed{};
    std::optional<ConfigurationFailure> lastFailure{};
};
struct AwaitingCandidate final {
    ConfigurationIdentity identity{};
    ConfigurationOrigin origin{ConfigurationOrigin::ControlClient};
    DeviceConfiguration requested{};
    CommittedConfiguration prior{};
};
struct AwaitingADKPerform final {
    TransitionContext transition{};
    PendingWork work{};
};
struct AwaitingHardware final {
    TransitionContext transition{};
};
struct AwaitingADKProjection final {
    TransitionContext transition{};
    ProjectionPlan plan{};
};
struct Recovering final {
    CommittedConfiguration lastCoherent{};
    std::optional<ConfigurationIdentity> failedIdentity{};
    ConfigurationFailure failure{};
};
struct Unavailable final {
    CommittedConfiguration lastCoherent{};
    ConfigurationFailure failure{};
};

using DeviceConfigurationState = std::variant<
    Uninitialized,
    Idle,
    AwaitingCandidate,
    AwaitingADKPerform,
    AwaitingHardware,
    AwaitingADKProjection,
    Recovering,
    Unavailable>;

struct Machine final {
    DeviceConfigurationState state{};
    TransactionToken nextToken{1};
};

enum class HardwareOutcomeKind : uint8_t {
    Unchanged,
    ConfirmedRequested,
    ConfirmedOther,
    Unknown,
};

struct HardwareUnchanged final {};
struct HardwareConfirmedRequested final {
    ConfirmedHardwareConfiguration confirmed{};
};
struct HardwareConfirmedOther final {
    ConfirmedHardwareConfiguration confirmed{};
};
struct HardwareUnknown final {};
using HardwareConfigurationOutcome = std::variant<
    HardwareUnchanged,
    HardwareConfirmedRequested,
    HardwareConfirmedOther,
    HardwareUnknown>;

struct ControlIntent final {
    EndpointId endpointId{0};
    RouteGeneration routeGeneration{0};
    DeviceConfiguration requested{};
};
struct CoreAudioRateIntent final {
    EndpointId endpointId{0};
    RouteGeneration routeGeneration{0};
    SampleRate sampleRate{0};
};
struct HardwareObserved final {
    EndpointId endpointId{0};
    RouteGeneration routeGeneration{0};
    ConfirmedHardwareConfiguration confirmed{};
};
struct CandidateAccepted final {
    ConfigurationIdentity identity{};
    DeviceConfiguration candidate{};
};
struct CandidateRejected final {
    ConfigurationIdentity identity{};
};
struct ADKPerformGranted final {
    ConfigurationIdentity identity{};
};
struct ADKWindowRejected final {
    ConfigurationIdentity identity{};
};
struct HardwareCompleted final {
    ConfigurationIdentity identity{};
    HardwareConfigurationOutcome outcome{};
};
struct ProjectionFinished final {
    ConfigurationIdentity identity{};
    bool customProjectionSucceeded{false};
    bool superclassSucceeded{false};
};
struct ADKAborted final {
    ConfigurationIdentity identity{};
};
struct RouteInvalidated final {
    EndpointId endpointId{0};
    RouteGeneration replacementGeneration{0};
};
struct RecoveryCompleted final {
    EndpointId endpointId{0};
    RouteGeneration routeGeneration{0};
    HardwareConfigurationOutcome outcome{};
};

using ConfigurationEvent = std::variant<
    ControlIntent,
    CoreAudioRateIntent,
    HardwareObserved,
    CandidateAccepted,
    CandidateRejected,
    ADKPerformGranted,
    ADKWindowRejected,
    HardwareCompleted,
    ProjectionFinished,
    ADKAborted,
    RouteInvalidated,
    RecoveryCompleted>;

struct ResolveCandidateEffect final {
    ConfigurationIdentity identity{};
    DeviceConfiguration requested{};
};
struct RequestADKWindowEffect final {
    ConfigurationIdentity identity{};
};
struct ApplyHardwareEffect final {
    TransitionContext transition{};
};
struct ProjectADKEffect final {
    ProjectionPlan plan{};
};
struct PublishSnapshotEffect final {
    CommittedConfiguration committed{};
};
struct QuiesceTransportEffect final {
    EndpointId endpointId{0};
};
struct ObserveHardwareEffect final {
    EndpointId endpointId{0};
    RouteGeneration routeGeneration{0};
};

using ConfigurationEffect = std::variant<
    ResolveCandidateEffect,
    RequestADKWindowEffect,
    ApplyHardwareEffect,
    ProjectADKEffect,
    PublishSnapshotEffect,
    QuiesceTransportEffect,
    ObserveHardwareEffect>;

template <typename T, size_t Capacity>
class FixedEffectList final {
public:
    [[nodiscard]] bool push(T effect) noexcept {
        if (size_ == Capacity) return false;
        effects_[size_++] = std::move(effect);
        return true;
    }

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] const T& operator[](size_t index) const noexcept {
        return effects_[index];
    }

private:
    std::array<T, Capacity> effects_{};
    size_t size_{0};
};

constexpr size_t kMaxEffectsPerTransition = 2;

enum class TransitionDisposition : uint8_t {
    Applied,
    NoOp,
};

struct TransitionResult final {
    Machine next{};
    FixedEffectList<ConfigurationEffect, kMaxEffectsPerTransition> effects{};
    TransitionDisposition disposition{TransitionDisposition::Applied};
};

enum class StateMachineError : uint8_t {
    InvalidEvent,
    EndpointMismatch,
    GenerationMismatch,
    StaleToken,
    Busy,
};

[[nodiscard]] bool SameConfiguration(
    const DeviceConfiguration& lhs,
    const DeviceConfiguration& rhs) noexcept;
[[nodiscard]] bool SameIdentity(
    const ConfigurationIdentity& lhs,
    const ConfigurationIdentity& rhs) noexcept;
[[nodiscard]] HardwareOutcomeKind HardwareKind(
    const HardwareConfigurationOutcome& outcome) noexcept;
[[nodiscard]] std::optional<CommittedConfiguration> CoherentSnapshot(
    const DeviceConfigurationState& state) noexcept;

[[nodiscard]] std::expected<TransitionResult, StateMachineError>
Reduce(const Machine& machine, const ConfigurationEvent& event) noexcept;

} // namespace ASFW::Configuration
