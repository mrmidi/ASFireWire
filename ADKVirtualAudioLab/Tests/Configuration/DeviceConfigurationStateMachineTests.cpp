#include "../TestHarness.hpp"
#include "../../Core/Configuration/DeviceConfigurationStateMachine.hpp"

#include <type_traits>
#include <variant>

namespace ASFW::LabTests {
namespace {

using namespace ASFW::Configuration;

constexpr EndpointId kEndpoint = 0x1814;
constexpr RouteGeneration kGeneration = 7;

::ASFW::Device::DeviceConfiguration Config(::ASFW::Device::SampleRate sampleRate,
                                           ::ASFW::Device::OpticalMode input = ::ASFW::Device::OpticalMode::Adat,
                                           ::ASFW::Device::OpticalMode output = ::ASFW::Device::OpticalMode::Adat) {
    return ::ASFW::Device::DeviceConfiguration{
        .sampleRate = sampleRate,
        .opticalInput = input,
        .opticalOutput = output,
    };
}

Machine Baseline() {
    return Machine{
        .state = Idle{
            .committed = CommittedConfiguration{
                .endpointId = kEndpoint,
                .routeGeneration = kGeneration,
                .revision = 1,
                .configuration = Config(48000),
            },
        },
        .nextToken = 1,
    };
}

ConfigurationIdentity CandidateIdentity(const Machine& machine) {
    const auto* state = std::get_if<AwaitingCandidate>(&machine.state);
    return state ? state->identity : ConfigurationIdentity{};
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

} // namespace

void RunDeviceConfigurationStateMachineTests(TestContext& ctx) {
    // A committed configuration rejects no state change for the same request,
    // and a new control request first resolves its candidate.
    {
        Machine machine = Baseline();
        const auto noOp = Reduce(machine, ConfigurationEvent{ControlIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .requested = Config(48000),
        }});
        REQUIRE(ctx, noOp.has_value());
        CHECK(ctx, noOp->disposition == TransitionDisposition::NoOp);
        CHECK(ctx, noOp->effects.empty());

        const auto staged = Reduce(machine, ConfigurationEvent{ControlIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .requested = Config(44100),
        }});
        REQUIRE(ctx, staged.has_value());
        machine = staged->next;
        CHECK(ctx, std::holds_alternative<AwaitingCandidate>(machine.state));
        CHECK(ctx, staged->effects.size() == 1);
        CHECK(ctx, std::holds_alternative<ResolveCandidateEffect>(staged->effects[0]));

        const auto identity = CandidateIdentity(machine);
        const auto rejected = Reduce(machine,
            ConfigurationEvent{CandidateRejected{.identity = identity}});
        REQUIRE(ctx, rejected.has_value());
        machine = rejected->next;
        const auto* idle = std::get_if<Idle>(&machine.state);
        REQUIRE(ctx, idle != nullptr);
        CHECK_EQ_U64(ctx, idle->committed.revision, 1);
        REQUIRE(ctx, idle->lastFailure.has_value());
        CHECK(ctx, idle->lastFailure->reason == FailureReason::CandidateRejected);
    }

    // Control-origin success requires candidate resolution, an ADK perform
    // window, hardware confirmation, then a successful projection commit.
    {
        Machine machine = Baseline();
        auto step = Reduce(machine, ConfigurationEvent{ControlIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .requested = Config(44100),
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto identity = CandidateIdentity(machine);

        step = Reduce(machine, ConfigurationEvent{CandidateAccepted{
            .identity = identity,
            .candidate = Config(44100),
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        CHECK(ctx, std::holds_alternative<AwaitingADKPerform>(machine.state));
        CHECK(ctx, step->effects.size() == 1);
        CHECK(ctx, std::holds_alternative<RequestADKWindowEffect>(step->effects[0]));

        step = Reduce(machine,
            ConfigurationEvent{ADKPerformGranted{.identity = identity}});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        CHECK(ctx, std::holds_alternative<AwaitingHardware>(machine.state));
        CHECK(ctx, step->effects.size() == 1);
        CHECK(ctx, std::holds_alternative<ApplyHardwareEffect>(step->effects[0]));

        step = Reduce(machine, ConfigurationEvent{HardwareCompleted{
            .identity = identity,
            .outcome = HardwareConfirmedRequested{
                .confirmed = ConfirmedHardwareConfiguration{.configuration = Config(44100)},
            },
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        CHECK(ctx, std::holds_alternative<AwaitingADKProjection>(machine.state));
        CHECK(ctx, step->effects.size() == 1);
        CHECK(ctx, std::holds_alternative<ProjectADKEffect>(step->effects[0]));

        step = Reduce(machine, ConfigurationEvent{ProjectionFinished{
            .identity = identity,
            .customProjectionSucceeded = true,
            .superclassSucceeded = true,
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto* idle = std::get_if<Idle>(&machine.state);
        REQUIRE(ctx, idle != nullptr);
        CHECK_EQ_U64(ctx, idle->committed.revision, 2);
        CHECK_EQ_U32(ctx, idle->committed.configuration.sampleRate, 44100);
        CHECK(ctx, step->effects.size() == 1);
        CHECK(ctx, std::holds_alternative<PublishSnapshotEffect>(step->effects[0]));
    }

    // Core Audio's validated rate callback requests an ADK window;
    // once granted it proceeds to hardware application.
    {
        Machine machine = Baseline();
        auto step = Reduce(machine, ConfigurationEvent{CoreAudioRateIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .sampleRate = 44100,
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto identity = CandidateIdentity(machine);

        step = Reduce(machine, ConfigurationEvent{CandidateAccepted{
            .identity = identity,
            .candidate = Config(44100),
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        CHECK(ctx, std::holds_alternative<AwaitingADKPerform>(machine.state));
        CHECK(ctx, step->effects.size() == 1);
        CHECK(ctx, std::holds_alternative<RequestADKWindowEffect>(step->effects[0]));

        step = Reduce(machine, ConfigurationEvent{ADKPerformGranted{.identity = identity}});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        CHECK(ctx, std::holds_alternative<AwaitingHardware>(machine.state));
        CHECK(ctx, step->effects.size() == 1);
        CHECK(ctx, std::holds_alternative<ApplyHardwareEffect>(step->effects[0]));
    }

    // A host-refused ADK window and a known-unchanged hardware rejection both
    // retain the last coherent revision rather than entering recovery.
    {
        Machine machine = Baseline();
        auto step = Reduce(machine, ConfigurationEvent{ControlIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .requested = Config(44100),
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto controlIdentity = CandidateIdentity(machine);
        step = Reduce(machine, ConfigurationEvent{CandidateAccepted{
            .identity = controlIdentity,
            .candidate = Config(44100),
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        step = Reduce(machine,
            ConfigurationEvent{ADKWindowRejected{.identity = controlIdentity}});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto* rejectedIdle = std::get_if<Idle>(&machine.state);
        REQUIRE(ctx, rejectedIdle != nullptr);
        CHECK_EQ_U64(ctx, rejectedIdle->committed.revision, 1);
        REQUIRE(ctx, rejectedIdle->lastFailure.has_value());
        CHECK(ctx, rejectedIdle->lastFailure->reason == FailureReason::ADKWindowRejected);

        machine = Baseline();
        step = Reduce(machine, ConfigurationEvent{CoreAudioRateIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .sampleRate = 44100,
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto coreAudioIdentity = CandidateIdentity(machine);
        step = Reduce(machine, ConfigurationEvent{CandidateAccepted{
            .identity = coreAudioIdentity,
            .candidate = Config(44100),
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        step = Reduce(machine, ConfigurationEvent{ADKPerformGranted{.identity = coreAudioIdentity}});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        step = Reduce(machine, ConfigurationEvent{HardwareCompleted{
            .identity = coreAudioIdentity,
            .outcome = HardwareUnchanged{},
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto* hardwareRejectedIdle = std::get_if<Idle>(&machine.state);
        REQUIRE(ctx, hardwareRejectedIdle != nullptr);
        CHECK_EQ_U64(ctx, hardwareRejectedIdle->committed.revision, 1);
        REQUIRE(ctx, hardwareRejectedIdle->lastFailure.has_value());
        CHECK(ctx, hardwareRejectedIdle->lastFailure->reason == FailureReason::HardwareRejected);
    }

    // A hardware-origin change asks ADK to synchronize but never rewrites the
    // already-observed hardware configuration.
    {
        Machine machine = Baseline();
        auto step = Reduce(machine, ConfigurationEvent{HardwareObserved{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .confirmed = ConfirmedHardwareConfiguration{
                .configuration = Config(48000, Device::OpticalMode::Spdif),
            },
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto identity = PendingIdentity(machine);
        CHECK(ctx, std::holds_alternative<AwaitingADKPerform>(machine.state));
        CHECK(ctx, std::holds_alternative<RequestADKWindowEffect>(step->effects[0]));

        step = Reduce(machine,
            ConfigurationEvent{ADKPerformGranted{.identity = identity}});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        CHECK(ctx, std::holds_alternative<AwaitingADKProjection>(machine.state));
        CHECK(ctx, step->effects.size() == 1);
        CHECK(ctx, std::holds_alternative<ProjectADKEffect>(step->effects[0]));
    }

    // Equal pending requests coalesce, different requests fail busy, and a
    // stale completion cannot advance or mutate the transaction.
    {
        Machine machine = Baseline();
        auto step = Reduce(machine, ConfigurationEvent{ControlIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .requested = Config(44100),
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto identity = CandidateIdentity(machine);

        const auto coalesced = Reduce(machine, ConfigurationEvent{ControlIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .requested = Config(44100),
        }});
        REQUIRE(ctx, coalesced.has_value());
        CHECK(ctx, coalesced->disposition == TransitionDisposition::Coalesced);

        const auto busy = Reduce(machine, ConfigurationEvent{ControlIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .requested = Config(96000),
        }});
        CHECK(ctx, !busy.has_value());
        CHECK(ctx, busy.error() == StateMachineError::Busy);

        const auto stale = Reduce(machine, ConfigurationEvent{CandidateAccepted{
            .identity = ConfigurationIdentity{
                .endpointId = kEndpoint,
                .token = identity.token + 1,
                .routeGeneration = kGeneration,
            },
            .candidate = Config(44100),
        }});
        CHECK(ctx, !stale.has_value());
        CHECK(ctx, stale.error() == StateMachineError::StaleToken);
        CHECK(ctx, std::holds_alternative<AwaitingCandidate>(machine.state));
    }

    // An unknown hardware outcome preserves the last coherent snapshot but
    // enters recovery, quiesces transport, and requests authoritative readback.
    {
        Machine machine = Baseline();
        auto step = Reduce(machine, ConfigurationEvent{CoreAudioRateIntent{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .sampleRate = 44100,
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto identity = CandidateIdentity(machine);
        step = Reduce(machine, ConfigurationEvent{CandidateAccepted{
            .identity = identity,
            .candidate = Config(44100),
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        REQUIRE(ctx, std::holds_alternative<AwaitingADKPerform>(machine.state));

        step = Reduce(machine, ConfigurationEvent{ADKPerformGranted{.identity = identity}});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        REQUIRE(ctx, std::holds_alternative<AwaitingHardware>(machine.state));

        step = Reduce(machine, ConfigurationEvent{HardwareCompleted{
            .identity = identity,
            .outcome = HardwareUnknown{},
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto* recovery = std::get_if<Recovering>(&machine.state);
        REQUIRE(ctx, recovery != nullptr);
        CHECK_EQ_U64(ctx, recovery->lastCoherent.revision, 1);
        CHECK(ctx, step->effects.size() == 2);
        CHECK(ctx, std::holds_alternative<QuiesceTransportEffect>(step->effects[0]));
        CHECK(ctx, std::holds_alternative<ObserveHardwareEffect>(step->effects[1]));

        step = Reduce(machine, ConfigurationEvent{RecoveryCompleted{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .outcome = HardwareConfirmedOther{
                .confirmed = ConfirmedHardwareConfiguration{.configuration = Config(44100)},
            },
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        CHECK(ctx, std::holds_alternative<AwaitingADKPerform>(machine.state));
        CHECK(ctx, std::holds_alternative<RequestADKWindowEffect>(step->effects[0]));
    }

    // A projection failure after hardware success and a route invalidation both
    // refuse to present a new configuration as committed.
    {
        Machine machine = Baseline();
        auto step = Reduce(machine, ConfigurationEvent{HardwareObserved{
            .endpointId = kEndpoint,
            .routeGeneration = kGeneration,
            .confirmed = ConfirmedHardwareConfiguration{.configuration = Config(44100)},
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        const auto identity = PendingIdentity(machine);
        step = Reduce(machine,
            ConfigurationEvent{ADKPerformGranted{.identity = identity}});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        step = Reduce(machine, ConfigurationEvent{ProjectionFinished{
            .identity = identity,
            .customProjectionSucceeded = false,
            .superclassSucceeded = true,
        }});
        REQUIRE(ctx, step.has_value());
        machine = step->next;
        CHECK(ctx, std::holds_alternative<Recovering>(machine.state));

        const auto invalidated = Reduce(machine, ConfigurationEvent{RouteInvalidated{
            .endpointId = kEndpoint,
            .replacementGeneration = kGeneration + 1,
        }});
        REQUIRE(ctx, invalidated.has_value());
        machine = invalidated->next;
        CHECK(ctx, std::holds_alternative<Unavailable>(machine.state));
        const auto coherent = CoherentSnapshot(machine.state);
        REQUIRE(ctx, coherent.has_value());
        CHECK_EQ_U64(ctx, coherent->revision, 1);
    }
}

} // namespace ASFW::LabTests
