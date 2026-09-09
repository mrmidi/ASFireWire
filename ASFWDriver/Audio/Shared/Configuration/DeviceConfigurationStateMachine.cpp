#include "DeviceConfigurationStateMachine.hpp"

#include <type_traits>

namespace ASFW::Configuration {
namespace {

template <typename T>
[[nodiscard]] bool Is(const DeviceConfigurationState& state) noexcept {
    return std::holds_alternative<T>(state);
}

[[nodiscard]] std::optional<ConfigurationIdentity>
PendingIdentity(const DeviceConfigurationState& state) noexcept {
    return std::visit([](const auto& value) -> std::optional<ConfigurationIdentity> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AwaitingCandidate>) {
            return value.identity;
        } else if constexpr (std::is_same_v<T, AwaitingADKPerform> ||
                             std::is_same_v<T, AwaitingHardware> ||
                             std::is_same_v<T, AwaitingADKProjection>) {
            return value.transition.identity;
        }
        return std::nullopt;
    }, state);
}

[[nodiscard]] const DeviceConfiguration*
PendingRequested(const DeviceConfigurationState& state) noexcept {
    return std::visit([](const auto& value) -> const DeviceConfiguration* {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AwaitingCandidate>) {
            return &value.requested;
        } else if constexpr (std::is_same_v<T, AwaitingADKPerform> ||
                             std::is_same_v<T, AwaitingHardware> ||
                             std::is_same_v<T, AwaitingADKProjection>) {
            return &value.transition.requested;
        }
        return nullptr;
    }, state);
}

[[nodiscard]] std::expected<TransitionResult, StateMachineError>
BusyOrCoalesced(const Machine& machine, const ControlIntent& intent) noexcept {
    const auto* pending = PendingRequested(machine.state);
    if (pending && SameConfiguration(*pending, intent.requested)) {
        return TransitionResult{.next = machine,
                                .disposition = TransitionDisposition::Coalesced};
    }
    return std::unexpected(StateMachineError::Busy);
}

[[nodiscard]] std::expected<TransitionResult, StateMachineError>
RequirePending(const Machine& machine, const ConfigurationIdentity& identity) noexcept {
    const auto pending = PendingIdentity(machine.state);
    if (!pending) return std::unexpected(StateMachineError::InvalidEvent);
    if (!SameIdentity(*pending, identity)) {
        return std::unexpected(StateMachineError::StaleToken);
    }
    return TransitionResult{.next = machine};
}

[[nodiscard]] std::expected<TransitionResult, StateMachineError>
MakeObservationTransition(const Machine& machine,
                          const HardwareObserved& observed,
                          const CommittedConfiguration& prior) noexcept {
    if (observed.endpointId != prior.endpointId) {
        return std::unexpected(StateMachineError::EndpointMismatch);
    }
    if (observed.routeGeneration != prior.routeGeneration) {
        return std::unexpected(StateMachineError::GenerationMismatch);
    }
    if (Is<Idle>(machine.state) &&
        SameConfiguration(prior.configuration, observed.confirmed.configuration)) {
        return TransitionResult{.next = machine,
                                .disposition = TransitionDisposition::NoOp};
    }

    Machine next = machine;
    const ConfigurationIdentity identity{
        .endpointId = observed.endpointId,
        .token = next.nextToken++,
        .routeGeneration = observed.routeGeneration,
    };
    TransitionContext transition{
        .identity = identity,
        .origin = ConfigurationOrigin::HardwareObservation,
        .requested = observed.confirmed.configuration,
        .candidate = observed.confirmed.configuration,
        .prior = prior,
    };
    next.state = AwaitingADKPerform{
        .transition = transition,
        .work = ProjectObserved{.confirmed = observed.confirmed},
    };
    TransitionResult result{.next = std::move(next)};
    (void)result.effects.push(RequestADKWindowEffect{.identity = identity});
    return result;
}

[[nodiscard]] std::expected<TransitionResult, StateMachineError>
StartRecovery(const Machine& machine, const ConfigurationIdentity& identity,
              const CommittedConfiguration& prior, FailureReason reason) noexcept {
    Machine next = machine;
    next.state = Recovering{
        .lastCoherent = prior,
        .failedIdentity = identity,
        .failure = ConfigurationFailure{.reason = reason},
    };
    TransitionResult result{.next = std::move(next)};
    (void)result.effects.push(QuiesceTransportEffect{.endpointId = prior.endpointId});
    (void)result.effects.push(ObserveHardwareEffect{
        .endpointId = prior.endpointId,
        .routeGeneration = prior.routeGeneration,
    });
    return result;
}

} // namespace

bool SameConfiguration(const DeviceConfiguration& lhs,
                       const DeviceConfiguration& rhs) noexcept {
    return lhs.sampleRate == rhs.sampleRate &&
           lhs.opticalInput == rhs.opticalInput &&
           lhs.opticalOutput == rhs.opticalOutput;
}

bool SameIdentity(const ConfigurationIdentity& lhs,
                  const ConfigurationIdentity& rhs) noexcept {
    return lhs.endpointId == rhs.endpointId && lhs.token == rhs.token &&
           lhs.routeGeneration == rhs.routeGeneration;
}

HardwareOutcomeKind HardwareKind(const HardwareConfigurationOutcome& outcome) noexcept {
    return std::visit([](const auto& value) -> HardwareOutcomeKind {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, HardwareUnchanged>) {
            return HardwareOutcomeKind::Unchanged;
        } else if constexpr (std::is_same_v<T, HardwareConfirmedRequested>) {
            return HardwareOutcomeKind::ConfirmedRequested;
        } else if constexpr (std::is_same_v<T, HardwareConfirmedOther>) {
            return HardwareOutcomeKind::ConfirmedOther;
        }
        return HardwareOutcomeKind::Unknown;
    }, outcome);
}

std::optional<CommittedConfiguration>
CoherentSnapshot(const DeviceConfigurationState& state) noexcept {
    return std::visit([](const auto& value) -> std::optional<CommittedConfiguration> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Idle>) {
            return value.committed;
        } else if constexpr (std::is_same_v<T, AwaitingCandidate>) {
            return value.prior;
        } else if constexpr (std::is_same_v<T, AwaitingADKPerform> ||
                             std::is_same_v<T, AwaitingHardware> ||
                             std::is_same_v<T, AwaitingADKProjection>) {
            return value.transition.prior;
        } else if constexpr (std::is_same_v<T, Recovering> ||
                             std::is_same_v<T, Unavailable>) {
            return value.lastCoherent;
        }
        return std::nullopt;
    }, state);
}

std::expected<TransitionResult, StateMachineError>
Reduce(const Machine& machine, const ConfigurationEvent& event) noexcept {
    return std::visit([&](const auto& value)
        -> std::expected<TransitionResult, StateMachineError> {
        using Event = std::decay_t<decltype(value)>;

        if constexpr (std::is_same_v<Event, ControlIntent>) {
            if (const auto* idle = std::get_if<Idle>(&machine.state)) {
                if (value.endpointId != idle->committed.endpointId) {
                    return std::unexpected(StateMachineError::EndpointMismatch);
                }
                if (value.routeGeneration != idle->committed.routeGeneration) {
                    return std::unexpected(StateMachineError::GenerationMismatch);
                }
                if (SameConfiguration(value.requested, idle->committed.configuration)) {
                    return TransitionResult{.next = machine,
                                            .disposition = TransitionDisposition::NoOp};
                }
                Machine next = machine;
                const ConfigurationIdentity identity{
                    .endpointId = value.endpointId,
                    .token = next.nextToken++,
                    .routeGeneration = value.routeGeneration,
                };
                next.state = AwaitingCandidate{
                    .identity = identity,
                    .origin = ConfigurationOrigin::ControlClient,
                    .requested = value.requested,
                    .prior = idle->committed,
                };
                TransitionResult result{.next = std::move(next)};
                (void)result.effects.push(ResolveCandidateEffect{
                    .identity = identity,
                    .requested = value.requested,
                });
                return result;
            }
            return BusyOrCoalesced(machine, value);
        } else if constexpr (std::is_same_v<Event, CoreAudioRateIntent>) {
            const auto coherent = CoherentSnapshot(machine.state);
            if (!coherent) return std::unexpected(StateMachineError::InvalidEvent);
            if (value.endpointId != coherent->endpointId) {
                return std::unexpected(StateMachineError::EndpointMismatch);
            }
            if (value.routeGeneration != coherent->routeGeneration) {
                return std::unexpected(StateMachineError::GenerationMismatch);
            }
            if (!Is<Idle>(machine.state)) return std::unexpected(StateMachineError::Busy);
            DeviceConfiguration requested = coherent->configuration;
            requested.sampleRate = value.sampleRate;
            if (SameConfiguration(requested, coherent->configuration)) {
                return TransitionResult{.next = machine,
                                        .disposition = TransitionDisposition::NoOp};
            }
            Machine next = machine;
            const ConfigurationIdentity identity{
                .endpointId = value.endpointId,
                .token = next.nextToken++,
                .routeGeneration = value.routeGeneration,
            };
            next.state = AwaitingCandidate{
                .identity = identity,
                .origin = ConfigurationOrigin::CoreAudio,
                .requested = requested,
                .prior = *coherent,
            };
            TransitionResult result{.next = std::move(next)};
            (void)result.effects.push(ResolveCandidateEffect{
                .identity = identity,
                .requested = requested,
            });
            return result;
        } else if constexpr (std::is_same_v<Event, HardwareObserved>) {
            const auto coherent = CoherentSnapshot(machine.state);
            if (!coherent) return std::unexpected(StateMachineError::InvalidEvent);
            return MakeObservationTransition(machine, value, *coherent);
        } else if constexpr (std::is_same_v<Event, CandidateAccepted>) {
            const auto valid = RequirePending(machine, value.identity);
            if (!valid) return std::unexpected(valid.error());
            const auto* pending = std::get_if<AwaitingCandidate>(&machine.state);
            if (!pending) return std::unexpected(StateMachineError::InvalidEvent);
            Machine next = machine;
            TransitionContext transition{
                .identity = pending->identity,
                .origin = pending->origin,
                .requested = pending->requested,
                .candidate = value.candidate,
                .prior = pending->prior,
            };
            next.state = AwaitingADKPerform{
                .transition = transition,
                .work = ApplyCandidate{},
            };
            TransitionResult result{.next = std::move(next)};
            (void)result.effects.push(RequestADKWindowEffect{.identity = transition.identity});
            return result;
        } else if constexpr (std::is_same_v<Event, CandidateRejected>) {
            const auto valid = RequirePending(machine, value.identity);
            if (!valid) return std::unexpected(valid.error());
            const auto* pending = std::get_if<AwaitingCandidate>(&machine.state);
            if (!pending) return std::unexpected(StateMachineError::InvalidEvent);
            Machine next = machine;
            next.state = Idle{
                .committed = pending->prior,
                .lastFailure = ConfigurationFailure{.reason = FailureReason::CandidateRejected},
            };
            return TransitionResult{.next = std::move(next)};
        } else if constexpr (std::is_same_v<Event, ADKWindowRejected>) {
            const auto valid = RequirePending(machine, value.identity);
            if (!valid) return std::unexpected(valid.error());
            const auto* pending = std::get_if<AwaitingADKPerform>(&machine.state);
            if (!pending) return std::unexpected(StateMachineError::InvalidEvent);
            Machine next = machine;
            next.state = Idle{
                .committed = pending->transition.prior,
                .lastFailure = ConfigurationFailure{.reason = FailureReason::ADKWindowRejected},
            };
            return TransitionResult{.next = std::move(next)};
        } else if constexpr (std::is_same_v<Event, ADKPerformGranted>) {
            const auto valid = RequirePending(machine, value.identity);
            if (!valid) return std::unexpected(valid.error());
            const auto* pending = std::get_if<AwaitingADKPerform>(&machine.state);
            if (!pending) return std::unexpected(StateMachineError::InvalidEvent);
            Machine next = machine;
            TransitionResult result{.next = std::move(next)};
            if (std::holds_alternative<ApplyCandidate>(pending->work)) {
                result.next.state = AwaitingHardware{.transition = pending->transition};
                (void)result.effects.push(ApplyHardwareEffect{.transition = pending->transition});
            } else {
                const auto& observed = std::get<ProjectObserved>(pending->work).confirmed;
                ProjectionPlan plan{
                    .identity = pending->transition.identity,
                    .prior = pending->transition.prior,
                    .confirmed = observed,
                };
                result.next.state = AwaitingADKProjection{
                    .transition = pending->transition,
                    .plan = plan,
                };
                (void)result.effects.push(ProjectADKEffect{.plan = plan});
            }
            return result;
        } else if constexpr (std::is_same_v<Event, HardwareCompleted>) {
            const auto valid = RequirePending(machine, value.identity);
            if (!valid) return std::unexpected(valid.error());
            const auto* pending = std::get_if<AwaitingHardware>(&machine.state);
            if (!pending) return std::unexpected(StateMachineError::InvalidEvent);
            const auto kind = HardwareKind(value.outcome);
            if (kind == HardwareOutcomeKind::Unchanged) {
                Machine next = machine;
                next.state = Idle{
                    .committed = pending->transition.prior,
                    .lastFailure = ConfigurationFailure{.reason = FailureReason::HardwareRejected},
                };
                return TransitionResult{.next = std::move(next)};
            }
            if (kind == HardwareOutcomeKind::Unknown) {
                return StartRecovery(machine, value.identity, pending->transition.prior,
                                     FailureReason::HardwareStateUnknown);
            }
            const auto confirmed = std::visit([](const auto& outcome)
                -> std::optional<ConfirmedHardwareConfiguration> {
                using T = std::decay_t<decltype(outcome)>;
                if constexpr (std::is_same_v<T, HardwareConfirmedRequested> ||
                              std::is_same_v<T, HardwareConfirmedOther>) {
                    return outcome.confirmed;
                }
                return std::nullopt;
            }, value.outcome);
            if (!confirmed) return std::unexpected(StateMachineError::InvalidEvent);
            Machine next = machine;
            ProjectionPlan plan{
                .identity = pending->transition.identity,
                .prior = pending->transition.prior,
                .confirmed = *confirmed,
            };
            next.state = AwaitingADKProjection{
                .transition = pending->transition,
                .plan = plan,
            };
            TransitionResult result{.next = std::move(next)};
            (void)result.effects.push(ProjectADKEffect{.plan = plan});
            return result;
        } else if constexpr (std::is_same_v<Event, ProjectionFinished>) {
            const auto valid = RequirePending(machine, value.identity);
            if (!valid) return std::unexpected(valid.error());
            const auto* pending = std::get_if<AwaitingADKProjection>(&machine.state);
            if (!pending) return std::unexpected(StateMachineError::InvalidEvent);
            if (!value.customProjectionSucceeded || !value.superclassSucceeded) {
                return StartRecovery(machine, value.identity, pending->transition.prior,
                                     FailureReason::ProjectionFailed);
            }
            Machine next = machine;
            CommittedConfiguration committed{
                .endpointId = pending->plan.prior.endpointId,
                .routeGeneration = pending->plan.identity.routeGeneration,
                .revision = pending->plan.prior.revision + 1,
                .configuration = pending->plan.confirmed.configuration,
            };
            next.state = Idle{.committed = committed};
            TransitionResult result{.next = std::move(next)};
            (void)result.effects.push(PublishSnapshotEffect{.committed = committed});
            return result;
        } else if constexpr (std::is_same_v<Event, ADKAborted>) {
            const auto valid = RequirePending(machine, value.identity);
            if (!valid) return std::unexpected(valid.error());
            if (const auto* candidate = std::get_if<AwaitingCandidate>(&machine.state)) {
                Machine next = machine;
                next.state = Idle{
                    .committed = candidate->prior,
                    .lastFailure = ConfigurationFailure{.reason = FailureReason::ADKAborted},
                };
                return TransitionResult{.next = std::move(next)};
            }
            if (const auto* window = std::get_if<AwaitingADKPerform>(&machine.state)) {
                Machine next = machine;
                next.state = Idle{
                    .committed = window->transition.prior,
                    .lastFailure = ConfigurationFailure{.reason = FailureReason::ADKAborted},
                };
                return TransitionResult{.next = std::move(next)};
            }
            const auto coherent = CoherentSnapshot(machine.state);
            if (!coherent) return std::unexpected(StateMachineError::InvalidEvent);
            return StartRecovery(machine, value.identity, *coherent, FailureReason::ADKAborted);
        } else if constexpr (std::is_same_v<Event, RouteInvalidated>) {
            const auto coherent = CoherentSnapshot(machine.state);
            if (!coherent) return std::unexpected(StateMachineError::InvalidEvent);
            if (value.endpointId != coherent->endpointId) {
                return std::unexpected(StateMachineError::EndpointMismatch);
            }
            Machine next = machine;
            next.state = Unavailable{
                .lastCoherent = *coherent,
                .failure = ConfigurationFailure{.reason = FailureReason::RouteInvalidated},
            };
            return TransitionResult{.next = std::move(next)};
        } else if constexpr (std::is_same_v<Event, RecoveryCompleted>) {
            const auto* recovery = std::get_if<Recovering>(&machine.state);
            if (!recovery) return std::unexpected(StateMachineError::InvalidEvent);
            if (value.endpointId != recovery->lastCoherent.endpointId) {
                return std::unexpected(StateMachineError::EndpointMismatch);
            }
            if (value.routeGeneration != recovery->lastCoherent.routeGeneration) {
                return std::unexpected(StateMachineError::GenerationMismatch);
            }
            const auto kind = HardwareKind(value.outcome);
            if (kind == HardwareOutcomeKind::Unchanged || kind == HardwareOutcomeKind::Unknown) {
                Machine next = machine;
                next.state = Unavailable{
                    .lastCoherent = recovery->lastCoherent,
                    .failure = ConfigurationFailure{.reason = FailureReason::HardwareStateUnknown},
                };
                return TransitionResult{.next = std::move(next)};
            }
            const auto confirmed = std::visit([](const auto& outcome)
                -> std::optional<ConfirmedHardwareConfiguration> {
                using T = std::decay_t<decltype(outcome)>;
                if constexpr (std::is_same_v<T, HardwareConfirmedRequested> ||
                              std::is_same_v<T, HardwareConfirmedOther>) {
                    return outcome.confirmed;
                }
                return std::nullopt;
            }, value.outcome);
            if (!confirmed) return std::unexpected(StateMachineError::InvalidEvent);
            Machine next = machine;
            const ConfigurationIdentity identity{
                .endpointId = value.endpointId,
                .token = next.nextToken++,
                .routeGeneration = value.routeGeneration,
            };
            TransitionContext transition{
                .identity = identity,
                .origin = ConfigurationOrigin::Recovery,
                .requested = confirmed->configuration,
                .candidate = confirmed->configuration,
                .prior = recovery->lastCoherent,
            };
            next.state = AwaitingADKPerform{
                .transition = transition,
                .work = ProjectObserved{.confirmed = *confirmed},
            };
            TransitionResult result{.next = std::move(next)};
            (void)result.effects.push(RequestADKWindowEffect{.identity = identity});
            return result;
        }
        return std::unexpected(StateMachineError::InvalidEvent);
    }, event);
}

} // namespace ASFW::Configuration
