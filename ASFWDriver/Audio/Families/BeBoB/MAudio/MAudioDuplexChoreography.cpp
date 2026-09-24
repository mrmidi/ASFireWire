// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MAudioDuplexChoreography.hpp"

namespace ASFW::Audio::Families::BeBoB::MAudio {

namespace {

[[nodiscard]] ChoreographyStep Stay(const ChoreographyState& state) noexcept {
    return ChoreographyStep{state, Done{}};
}

[[nodiscard]] StartEpoch StateEpoch(const ChoreographyState& state) noexcept {
    if (const auto* configuring = std::get_if<Configuring>(&state)) {
        return configuring->epoch;
    }
    if (const auto* connecting = std::get_if<Connecting>(&state)) {
        return connecting->epoch;
    }
    if (const auto* arming = std::get_if<ArmingHostTransport>(&state)) {
        return arming->epoch;
    }
    if (const auto* running = std::get_if<RunningTransport>(&state)) {
        return running->epoch;
    }
    if (const auto* stopped = std::get_if<Stopped>(&state)) {
        return stopped->epoch;
    }
    if (const auto* failed = std::get_if<Failed>(&state)) {
        return failed->epoch;
    }
    return {};
}

[[nodiscard]] const StartEpoch* EventEpoch(
    const ChoreographyEvent& event) noexcept {
    if (const auto* value =
            std::get_if<PreStreamConfigurationSucceeded>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<PreStreamConfigurationFailed>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<DeviceStreamsConnected>(&event)) {
        return &value->epoch;
    }
    if (const auto* value =
            std::get_if<DeviceStreamsConnectionFailed>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<HostTransportArmed>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<HostTransportArmFailed>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<DeviceStartConfirmed>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<DeviceStartRejected>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<TxGroupTimestampObserved>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<RxHeaderOnlyNoDataObserved>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<RxBecameUsable>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<CaptureStartExpired>(&event)) {
        return &value->epoch;
    }
    if (const auto* value = std::get_if<StopRequested>(&event)) {
        return &value->epoch;
    }
    return nullptr;
}

[[nodiscard]] bool EventMatchesStateEpoch(const ChoreographyState& state,
                                           const ChoreographyEvent& event) noexcept {
    const auto* eventEpoch = EventEpoch(event);
    return eventEpoch != nullptr && *eventEpoch == StateEpoch(state);
}

[[nodiscard]] bool HostTransportMayBeLive(
    const ChoreographyState& state) noexcept {
    return std::holds_alternative<ArmingHostTransport>(state) ||
           std::holds_alternative<RunningTransport>(state);
}

[[nodiscard]] ChoreographyStep Stop(const ChoreographyState& state) noexcept {
    const StartEpoch epoch = StateEpoch(state);
    if (HostTransportMayBeLive(state)) {
        return ChoreographyStep{Stopped{epoch}, StopHostTransport{}};
    }
    // Before the host-start action, stale epoch filtering is enough to make
    // pending FCP/CMP callbacks harmless. There is no DMA context to quiesce.
    return ChoreographyStep{Stopped{epoch}, Done{}};
}

[[nodiscard]] ChoreographyStep FailBeforeHostStart(StartEpoch epoch) noexcept {
    return ChoreographyStep{Failed{epoch}, FailStart{}};
}

[[nodiscard]] ChoreographyStep FailAfterHostStart(StartEpoch epoch) noexcept {
    return ChoreographyStep{Failed{epoch}, StopHostTransport{}};
}

[[nodiscard]] RunningTransport MakeRunning(
    const ArmingHostTransport& arming) noexcept {
    RunningTransport running{};
    running.epoch = arming.epoch;
    running.deviceStart = DeviceStartConfirming{};
    running.playback = arming.playback;
    running.capture = arming.capture;
    return running;
}

struct PlaybackAdvance final {
    PlaybackState playback;
    ChoreographyAction action;
};

[[nodiscard]] PlaybackAdvance AdvancePlayback(
    const PlaybackState& state,
    const TxGroupTimestampObserved& event) noexcept {
    if (std::holds_alternative<AwaitingCaptureReference>(state)) {
        if (event.groupCounter != kCaptureReferenceGroup) {
            return PlaybackAdvance{state, Done{}};
        }
        return PlaybackAdvance{AwaitingPlaybackAnchor{event.anchor},
                               PlantCaptureReference{event.anchor}};
    }

    if (std::holds_alternative<AwaitingPlaybackAnchor>(state)) {
        if (event.groupCounter < kFirstPlaybackClockGroup) {
            return PlaybackAdvance{state, Done{}};
        }
        return PlaybackAdvance{PlaybackRunning{event.anchor},
                               PublishPlaybackClock{event.anchor}};
    }

    // Once running, every completed TX group refreshes the TX-derived clock.
    return PlaybackAdvance{PlaybackRunning{event.anchor},
                           PublishPlaybackClock{event.anchor}};
}

struct CaptureAdvance final {
    CaptureState capture;
    ChoreographyAction action;
};

[[nodiscard]] CaptureAdvance AdvanceCapture(
    const CaptureState& state,
    const ChoreographyEvent& event) noexcept {
    if (std::holds_alternative<RxHeaderOnlyNoDataObserved>(event)) {
        if (!std::holds_alternative<CaptureAwaitingData>(state)) {
            return CaptureAdvance{state, Done{}};
        }
        return CaptureAdvance{CaptureInNoDataTransition{},
                              NoteCaptureNoDataTransition{}};
    }

    if (std::holds_alternative<RxBecameUsable>(event)) {
        if (std::holds_alternative<CaptureReady>(state)) {
            return CaptureAdvance{state, Done{}};
        }
        return CaptureAdvance{CaptureReady{}, MarkCaptureReady{}};
    }

    if (std::holds_alternative<CaptureStartExpired>(event)) {
        // The timer is intentionally allowed to race the first usable packet;
        // once capture is ready, a late expiry must not turn a healthy stream
        // back into a degraded one.
        if (std::holds_alternative<CaptureReady>(state) ||
            std::holds_alternative<CaptureDegraded>(state)) {
            return CaptureAdvance{state, Done{}};
        }
        // This is deliberately not StopHostTransport: the vendor's playback
        // clock is TX-derived, while an 1814 may spend a valid initial period
        // sending header-only capture traffic.
        return CaptureAdvance{CaptureDegraded{}, MarkCaptureDegraded{}};
    }

    return CaptureAdvance{state, Done{}};
}

[[nodiscard]] ChoreographyStep AdvanceArming(
    const ArmingHostTransport& state,
    const ChoreographyEvent& event) noexcept {
    if (std::holds_alternative<HostTransportArmed>(event)) {
        return ChoreographyStep{MakeRunning(state), ConfirmDeviceStart{}};
    }
    if (std::holds_alternative<HostTransportArmFailed>(event)) {
        return FailAfterHostStart(state.epoch);
    }

    ArmingHostTransport next = state;
    if (const auto* tx = std::get_if<TxGroupTimestampObserved>(&event)) {
        const auto update = AdvancePlayback(state.playback, *tx);
        next.playback = update.playback;
        return ChoreographyStep{next, update.action};
    }

    const auto update = AdvanceCapture(state.capture, event);
    next.capture = update.capture;
    return ChoreographyStep{next, update.action};
}

[[nodiscard]] ChoreographyStep AdvanceRunning(
    const RunningTransport& state,
    const ChoreographyEvent& event) noexcept {
    if (std::holds_alternative<DeviceStartRejected>(event)) {
        if (std::holds_alternative<DeviceStartConfirming>(state.deviceStart)) {
            return FailAfterHostStart(state.epoch);
        }
        return Stay(ChoreographyState{state});
    }

    if (std::holds_alternative<DeviceStartConfirmed>(event)) {
        if (!std::holds_alternative<DeviceStartConfirming>(state.deviceStart)) {
            return Stay(ChoreographyState{state});
        }
        RunningTransport next = state;
        next.deviceStart = DeviceStartReady{};
        return ChoreographyStep{next, Done{}};
    }

    if (const auto* tx = std::get_if<TxGroupTimestampObserved>(&event)) {
        RunningTransport next = state;
        const auto update = AdvancePlayback(state.playback, *tx);
        next.playback = update.playback;
        return ChoreographyStep{next, update.action};
    }

    RunningTransport next = state;
    const auto update = AdvanceCapture(state.capture, event);
    next.capture = update.capture;
    return ChoreographyStep{next, update.action};
}

} // namespace

ChoreographyStep Begin(StartEpoch epoch) noexcept {
    return ChoreographyStep{Configuring{epoch}, ApplyPreStreamConfiguration{}};
}

ChoreographyStep Advance(const ChoreographyState& state,
                         const ChoreographyEvent& event) noexcept {
    // A new FireWire generation invalidates the route behind every in-flight
    // completion. It wins over all other events and never permits a new write.
    if (std::holds_alternative<GenerationInvalidated>(event)) {
        if (IsTerminal(state)) {
            return Stay(state);
        }
        return Stop(state);
    }

    if (IsTerminal(state) || !EventMatchesStateEpoch(state, event)) {
        return Stay(state);
    }

    if (std::holds_alternative<StopRequested>(event)) {
        return Stop(state);
    }

    if (const auto* configuring = std::get_if<Configuring>(&state)) {
        if (std::holds_alternative<PreStreamConfigurationSucceeded>(event)) {
            return ChoreographyStep{Connecting{configuring->epoch},
                                     ConnectDeviceStreams{}};
        }
        if (std::holds_alternative<PreStreamConfigurationFailed>(event)) {
            return FailBeforeHostStart(configuring->epoch);
        }
        return Stay(state);
    }

    if (const auto* connecting = std::get_if<Connecting>(&state)) {
        if (std::holds_alternative<DeviceStreamsConnected>(event)) {
            return ChoreographyStep{ArmingHostTransport{connecting->epoch},
                                     StartAsymmetricHostTransport{}};
        }
        if (std::holds_alternative<DeviceStreamsConnectionFailed>(event)) {
            return FailBeforeHostStart(connecting->epoch);
        }
        return Stay(state);
    }

    if (const auto* arming = std::get_if<ArmingHostTransport>(&state)) {
        return AdvanceArming(*arming, event);
    }

    if (const auto* running = std::get_if<RunningTransport>(&state)) {
        return AdvanceRunning(*running, event);
    }

    return Stay(state);
}

const char* StateName(const ChoreographyState& state) noexcept {
    if (std::holds_alternative<Configuring>(state)) return "configuring";
    if (std::holds_alternative<Connecting>(state)) return "connecting";
    if (std::holds_alternative<ArmingHostTransport>(state)) {
        return "arming-host-transport";
    }
    if (std::holds_alternative<RunningTransport>(state)) {
        return "running-transport";
    }
    if (std::holds_alternative<Stopped>(state)) return "stopped";
    if (std::holds_alternative<Failed>(state)) return "failed";
    return "unknown";
}

const char* PlaybackStateName(const PlaybackState& state) noexcept {
    if (std::holds_alternative<AwaitingCaptureReference>(state)) {
        return "awaiting-capture-reference";
    }
    if (std::holds_alternative<AwaitingPlaybackAnchor>(state)) {
        return "awaiting-playback-anchor";
    }
    if (std::holds_alternative<PlaybackRunning>(state)) {
        return "playback-running";
    }
    return "unknown";
}

const char* CaptureStateName(const CaptureState& state) noexcept {
    if (std::holds_alternative<CaptureAwaitingData>(state)) {
        return "awaiting-data";
    }
    if (std::holds_alternative<CaptureInNoDataTransition>(state)) {
        return "header-only-transition";
    }
    if (std::holds_alternative<CaptureReady>(state)) return "ready";
    if (std::holds_alternative<CaptureDegraded>(state)) return "degraded";
    return "unknown";
}

const char* DeviceStartStateName(const DeviceStartState& state) noexcept {
    if (std::holds_alternative<DeviceStartConfirming>(state)) {
        return "confirming";
    }
    if (std::holds_alternative<DeviceStartReady>(state)) return "ready";
    return "unknown";
}

bool IsTerminal(const ChoreographyState& state) noexcept {
    return std::holds_alternative<Stopped>(state) ||
           std::holds_alternative<Failed>(state);
}

bool IsPlaybackRunning(const ChoreographyState& state) noexcept {
    const auto* running = std::get_if<RunningTransport>(&state);
    return running != nullptr &&
           std::holds_alternative<PlaybackRunning>(running->playback);
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
