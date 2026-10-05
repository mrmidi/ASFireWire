#include "Audio/Runtime/Configuration/DeviceConfigurationStateMachine.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace ASFW::Configuration {
namespace {

constexpr EndpointId kEndpointId = 0x1814;
constexpr RouteGeneration kGeneration = 7;

DeviceConfiguration Config(SampleRate sampleRate,
                           OpticalMode input = OpticalMode::Spdif,
                           OpticalMode output = OpticalMode::Spdif) {
    auto resolved = std::make_shared<Audio::Runtime::ResolvedAudioConfiguration>();
    resolved->formation.sampleRateHz = sampleRate;
    return DeviceConfiguration{
        .sampleRate = sampleRate,
        .opticalInput = input,
        .opticalOutput = output,
        .resolved = std::move(resolved),
    };
}

Machine Baseline() {
    return Machine{
        .state = Idle{.committed = CommittedConfiguration{
            .endpointId = kEndpointId,
            .routeGeneration = kGeneration,
            .revision = 4,
            .configuration = Config(48'000),
        }},
        .nextToken = 1,
    };
}

ConfigurationIdentity PendingIdentity(const Machine& machine) {
    return std::visit([](const auto& state) -> ConfigurationIdentity {
        using State = std::decay_t<decltype(state)>;
        if constexpr (std::is_same_v<State, AwaitingCandidate>) {
            return state.identity;
        } else if constexpr (std::is_same_v<State, AwaitingADKPerform> ||
                             std::is_same_v<State, AwaitingHardware> ||
                             std::is_same_v<State, AwaitingADKProjection>) {
            return state.transition.identity;
        }
        return {};
    }, machine.state);
}

TEST(DeviceConfigurationStateMachineTests, ControlIntentCommitsOnlyAfterProjection) {
    Machine machine = Baseline();
    auto transition = Reduce(machine, ConfigurationEvent{ControlIntent{
        .endpointId = kEndpointId,
        .routeGeneration = kGeneration,
        .requested = Config(44'100, OpticalMode::Adat, OpticalMode::Spdif),
    }});
    ASSERT_TRUE(transition);
    machine = transition->next;
    const auto identity = PendingIdentity(machine);
    ASSERT_TRUE(std::holds_alternative<ResolveCandidateEffect>(transition->effects[0]));

    transition = Reduce(machine, ConfigurationEvent{CandidateAccepted{
        .identity = identity,
        .candidate = Config(44'100, OpticalMode::Adat, OpticalMode::Spdif),
    }});
    ASSERT_TRUE(transition);
    machine = transition->next;
    ASSERT_TRUE(std::holds_alternative<RequestADKWindowEffect>(transition->effects[0]));

    transition = Reduce(machine, ConfigurationEvent{ADKPerformGranted{.identity = identity}});
    ASSERT_TRUE(transition);
    machine = transition->next;
    ASSERT_TRUE(std::holds_alternative<ApplyHardwareEffect>(transition->effects[0]));

    transition = Reduce(machine, ConfigurationEvent{HardwareCompleted{
        .identity = identity,
        .outcome = HardwareConfirmedRequested{
            .confirmed = ConfirmedHardwareConfiguration{
                .configuration = Config(44'100, OpticalMode::Adat, OpticalMode::Spdif)}}},
    });
    ASSERT_TRUE(transition);
    machine = transition->next;
    ASSERT_TRUE(std::holds_alternative<ProjectADKEffect>(transition->effects[0]));

    transition = Reduce(machine, ConfigurationEvent{ProjectionFinished{
        .identity = identity,
        .customProjectionSucceeded = true,
        .superclassSucceeded = true,
    }});
    ASSERT_TRUE(transition);
    const auto* idle = std::get_if<Idle>(&transition->next.state);
    ASSERT_NE(idle, nullptr);
    EXPECT_EQ(idle->committed.revision, 5U);
    EXPECT_EQ(idle->committed.configuration.sampleRate, 44'100U);
    ASSERT_TRUE(std::holds_alternative<PublishSnapshotEffect>(transition->effects[0]));
}

TEST(DeviceConfigurationStateMachineTests, CoreAudioRateIntentUsesGrantedWindow) {
    Machine machine = Baseline();
    auto transition = Reduce(machine, ConfigurationEvent{CoreAudioRateIntent{
        .endpointId = kEndpointId,
        .routeGeneration = kGeneration,
        .sampleRate = 44'100,
    }});
    ASSERT_TRUE(transition);
    machine = transition->next;
    const auto identity = PendingIdentity(machine);

    transition = Reduce(machine, ConfigurationEvent{CandidateAccepted{
        .identity = identity,
        .candidate = Config(44'100),
    }});
    ASSERT_TRUE(transition);
    EXPECT_TRUE(std::holds_alternative<AwaitingADKPerform>(transition->next.state));
    ASSERT_TRUE(std::holds_alternative<RequestADKWindowEffect>(transition->effects[0]));

    transition = Reduce(transition->next, ConfigurationEvent{ADKPerformGranted{.identity = identity}});
    ASSERT_TRUE(transition);
    EXPECT_TRUE(std::holds_alternative<AwaitingHardware>(transition->next.state));
    ASSERT_TRUE(std::holds_alternative<ApplyHardwareEffect>(transition->effects[0]));
}

TEST(DeviceConfigurationStateMachineTests, HardwareObservationProjectsWithoutApply) {
    const auto transition = Reduce(Baseline(), ConfigurationEvent{HardwareObserved{
        .endpointId = kEndpointId,
        .routeGeneration = kGeneration,
        .confirmed = ConfirmedHardwareConfiguration{.configuration = Config(44'100)},
    }});
    ASSERT_TRUE(transition);
    EXPECT_TRUE(std::holds_alternative<AwaitingADKPerform>(transition->next.state));
    ASSERT_TRUE(std::holds_alternative<RequestADKWindowEffect>(transition->effects[0]));
}

TEST(DeviceConfigurationStateMachineTests, UnknownHardwareStateQuiescesAndObserves) {
    Machine machine = Baseline();
    auto transition = Reduce(machine, ConfigurationEvent{CoreAudioRateIntent{
        .endpointId = kEndpointId,
        .routeGeneration = kGeneration,
        .sampleRate = 44'100,
    }});
    ASSERT_TRUE(transition);
    machine = transition->next;
    const auto identity = PendingIdentity(machine);
    transition = Reduce(machine, ConfigurationEvent{CandidateAccepted{
        .identity = identity,
        .candidate = Config(44'100),
    }});
    ASSERT_TRUE(transition);
    machine = transition->next;

    transition = Reduce(machine, ConfigurationEvent{ADKPerformGranted{.identity = identity}});
    ASSERT_TRUE(transition);
    machine = transition->next;

    transition = Reduce(machine, ConfigurationEvent{HardwareCompleted{
        .identity = identity,
        .outcome = HardwareUnknown{},
    }});
    ASSERT_TRUE(transition);
    EXPECT_TRUE(std::holds_alternative<Recovering>(transition->next.state));
    ASSERT_EQ(transition->effects.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<QuiesceTransportEffect>(transition->effects[0]));
    EXPECT_TRUE(std::holds_alternative<ObserveHardwareEffect>(transition->effects[1]));
}

TEST(DeviceConfigurationStateMachineTests, RejectsStaleCompletionToken) {
    Machine machine = Baseline();
    auto transition = Reduce(machine, ConfigurationEvent{ControlIntent{
        .endpointId = kEndpointId,
        .routeGeneration = kGeneration,
        .requested = Config(44'100),
    }});
    ASSERT_TRUE(transition);
    auto identity = PendingIdentity(transition->next);
    ++identity.token;

    const auto stale = Reduce(transition->next, ConfigurationEvent{CandidateRejected{
        .identity = identity,
    }});
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error(), StateMachineError::StaleToken);
}

TEST(DeviceConfigurationStateMachineTests, RapidSuccessiveRequestsYieldBusy) {
    Machine machine = Baseline();
    auto transition = Reduce(machine, ConfigurationEvent{CoreAudioRateIntent{
        .endpointId = kEndpointId,
        .routeGeneration = kGeneration,
        .sampleRate = 96'000,
    }});
    ASSERT_TRUE(transition);
    machine = transition->next;
    EXPECT_TRUE(std::holds_alternative<AwaitingCandidate>(machine.state));

    // A second rate intent while the first is in-flight must be rejected as Busy.
    const auto second = Reduce(machine, ConfigurationEvent{CoreAudioRateIntent{
        .endpointId = kEndpointId,
        .routeGeneration = kGeneration,
        .sampleRate = 48'000,
    }});
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error(), StateMachineError::Busy);
}

TEST(DeviceConfigurationStateMachineTests, AbortedADKWindowRollsBackToPriorCommitted) {
    Machine machine = Baseline();
    auto transition = Reduce(machine, ConfigurationEvent{CoreAudioRateIntent{
        .endpointId = kEndpointId,
        .routeGeneration = kGeneration,
        .sampleRate = 96'000,
    }});
    ASSERT_TRUE(transition);
    machine = transition->next;
    const auto identity = PendingIdentity(machine);

    transition = Reduce(machine, ConfigurationEvent{CandidateAccepted{
        .identity = identity,
        .candidate = Config(96'000),
    }});
    ASSERT_TRUE(transition);
    machine = transition->next;
    EXPECT_TRUE(std::holds_alternative<AwaitingADKPerform>(machine.state));

    // Simulate ADK denying/rejecting the configuration window.
    transition = Reduce(machine, ConfigurationEvent{ADKWindowRejected{
        .identity = identity,
    }});
    ASSERT_TRUE(transition);
    const auto* idle = std::get_if<Idle>(&transition->next.state);
    ASSERT_NE(idle, nullptr);
    EXPECT_EQ(idle->committed.revision, 4U);
    EXPECT_EQ(idle->committed.configuration.sampleRate, 48'000U);
    ASSERT_TRUE(idle->lastFailure.has_value());
    EXPECT_EQ(idle->lastFailure->reason, FailureReason::ADKWindowRejected);
}


Machine Applying(uint32_t rate = 96000) {
    auto requested = Reduce(Baseline(), CoreAudioRateIntent{kEndpointId, kGeneration, rate});
    const auto identity = PendingIdentity(requested->next);
    auto accepted = Reduce(requested->next, CandidateAccepted{identity, Config(rate)});
    return Reduce(accepted->next, ADKPerformGranted{identity})->next;
}

TEST(DeviceConfigurationStateMachineTests, ConfirmedOtherProjectsActualRate) {
    auto machine = Applying();
    auto identity = PendingIdentity(machine);
    auto result = Reduce(machine, HardwareCompleted{identity,
        HardwareConfirmedOther{{Config(44100)}}});
    ASSERT_TRUE(result);
    auto projection = std::get<ProjectADKEffect>(result->effects[0]);
    EXPECT_EQ(projection.plan.confirmed.configuration.sampleRate, 44100);
    auto committed = Reduce(result->next, ProjectionFinished{identity, true, true});
    ASSERT_TRUE(committed);
    EXPECT_EQ(CoherentSnapshot(committed->next.state)->configuration.sampleRate, 44100);
}

TEST(DeviceConfigurationStateMachineTests, ClaimedRequestedRateMustMatchCandidate) {
    auto machine = Applying();
    auto result = Reduce(machine, HardwareCompleted{PendingIdentity(machine),
        HardwareConfirmedRequested{{Config(44100)}}});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), StateMachineError::InvalidEvent);
}

TEST(DeviceConfigurationStateMachineTests, FailedProjectionNeverPublishesCandidate) {
    for (const auto outcome : {std::pair{false, true}, std::pair{true, false}}) {
        auto machine = Applying();
        auto identity = PendingIdentity(machine);
        auto confirmed = Reduce(machine, HardwareCompleted{identity,
            HardwareConfirmedRequested{{Config(96000)}}});
        ASSERT_TRUE(confirmed);
        auto failed = Reduce(confirmed->next, ProjectionFinished{identity, outcome.first, outcome.second});
        ASSERT_TRUE(failed);
        EXPECT_TRUE(std::holds_alternative<Recovering>(failed->next.state));
        EXPECT_EQ(CoherentSnapshot(failed->next.state)->configuration.sampleRate, 48000);
        ASSERT_EQ(failed->effects.size(), 2);
        EXPECT_TRUE(std::holds_alternative<QuiesceTransportEffect>(failed->effects[0]));
    }
}

TEST(DeviceConfigurationStateMachineTests, InvalidatedRouteRejectsCompletion) {
    auto machine = Applying();
    auto identity = PendingIdentity(machine);
    auto invalidated = Reduce(machine, RouteInvalidated{kEndpointId, kGeneration + 1});
    ASSERT_TRUE(invalidated);
    EXPECT_TRUE(std::holds_alternative<Unavailable>(invalidated->next.state));
    auto late = Reduce(invalidated->next, HardwareCompleted{identity, HardwareConfirmedRequested{{Config(96000)}}});
    EXPECT_FALSE(late);
    EXPECT_EQ(CoherentSnapshot(invalidated->next.state)->configuration.sampleRate, 48000);
}

TEST(DeviceConfigurationStateMachineTests, AbortAfterHardwareStartsRequiresRecovery) {
    auto machine = Applying();
    auto aborted = Reduce(machine, ADKAborted{PendingIdentity(machine)});
    ASSERT_TRUE(aborted);
    EXPECT_TRUE(std::holds_alternative<Recovering>(aborted->next.state));
}

TEST(DeviceConfigurationStateMachineTests, VerifiedUnchangedRetainsPriorRevision) {
    auto machine = Applying();
    auto result = Reduce(machine, HardwareCompleted{PendingIdentity(machine), HardwareUnchanged{}});
    ASSERT_TRUE(result);
    EXPECT_TRUE(std::holds_alternative<Idle>(result->next.state));
    EXPECT_EQ(CoherentSnapshot(result->next.state)->revision, 4);
    EXPECT_TRUE(result->effects.empty());
}

TEST(DeviceConfigurationStateMachineTests, FailedRecoveryBecomesUnavailable) {
    auto machine = Applying();
    auto failed = Reduce(machine, HardwareCompleted{PendingIdentity(machine), HardwareUnknown{}});
    ASSERT_TRUE(failed);
    auto exhausted = Reduce(failed->next, RecoveryCompleted{kEndpointId, kGeneration, HardwareUnknown{}});
    ASSERT_TRUE(exhausted);
    EXPECT_TRUE(std::holds_alternative<Unavailable>(exhausted->next.state));
}

TEST(DeviceConfigurationStateMachineTests, ScalarCandidateCannotBypassResolvedSnapshot) {
    auto request = Reduce(Baseline(), CoreAudioRateIntent{kEndpointId, kGeneration, 96000});
    auto candidate = Config(96000);
    candidate.resolved.reset();
    auto result = Reduce(request->next, CandidateAccepted{PendingIdentity(request->next), candidate});
    EXPECT_FALSE(result);
}


TEST(DeviceConfigurationStateMachineTests, SameRateDifferentLayoutRequiresProjection) {
    auto changed = Config(48000);
    auto snapshot = std::make_shared<Audio::Runtime::ResolvedAudioConfiguration>(*changed.resolved);
    snapshot->formation.capture.push_back({12, 13, 1, {}});
    changed.resolved = snapshot;
    auto result = Reduce(Baseline(), HardwareObserved{kEndpointId, kGeneration, {changed}});
    ASSERT_TRUE(result);
    EXPECT_TRUE(std::holds_alternative<AwaitingADKPerform>(result->next.state));
    EXPECT_EQ(result->effects.size(), 1);
}

TEST(DeviceConfigurationStateMachineTests, ObservationRequiresCompleteSnapshot) {
    auto scalar = Config(96000);
    scalar.resolved.reset();
    EXPECT_FALSE(Reduce(Baseline(), HardwareObserved{kEndpointId, kGeneration, {scalar}}));
    auto applying = Applying();
    auto failed = Reduce(applying, HardwareCompleted{PendingIdentity(applying), HardwareUnknown{}});
    ASSERT_TRUE(failed);
    EXPECT_FALSE(Reduce(failed->next, RecoveryCompleted{kEndpointId, kGeneration,
        HardwareConfirmedOther{{scalar}}}));
}

TEST(DeviceConfigurationStateMachineTests, ObservationCannotReplacePendingTransaction) {
    auto applying = Applying();
    auto observed = Reduce(applying, HardwareObserved{kEndpointId, kGeneration, {Config(44100)}});
    ASSERT_FALSE(observed);
    EXPECT_EQ(observed.error(), StateMachineError::Busy);
}

} // namespace
} // namespace ASFW::Configuration
