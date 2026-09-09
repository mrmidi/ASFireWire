#include "ASFWDriver/Audio/Shared/Configuration/DeviceConfigurationStateMachine.hpp"

#include <gtest/gtest.h>

#include <variant>

namespace ASFW::Configuration {
namespace {

constexpr EndpointId kEndpointId = 0x1814;
constexpr RouteGeneration kGeneration = 7;

DeviceConfiguration Config(SampleRate sampleRate,
                           OpticalMode input = OpticalMode::Spdif,
                           OpticalMode output = OpticalMode::Spdif) {
    return DeviceConfiguration{
        .sampleRate = sampleRate,
        .opticalInput = input,
        .opticalOutput = output,
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

} // namespace
} // namespace ASFW::Configuration
