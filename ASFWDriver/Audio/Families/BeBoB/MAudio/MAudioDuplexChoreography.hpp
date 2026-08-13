// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioDuplexChoreography.hpp -- pure start and sync policy for the M-Audio
// FireWire 1814 / ProjectMix special firmware.
//
// This is deliberately a decision machine, not a transport controller. It
// owns no FCP object, timer, DMA context, or AudioDriverKit object. An adapter
// feeds it completion/callback events and executes the returned action. That
// keeps the unusual M-Audio ordering host-testable before it is connected to
// the generic BeBoB duplex coordinator.
//
// There is intentionally no state enum or collection of "started" flags.
// Mutually exclusive states are std::variant alternatives, so a playback clock
// cannot be running without a TX anchor and the capture-reference warm-up
// cannot be skipped accidentally. The nested lanes in the host-start and
// running states are independent by design:
//
//   * device-start confirmation is an M-Audio FCP/rate re-assertion;
//   * playback readiness comes only from the TX timing callback;
//   * capture health may remain in an expected header-only NO-DATA transition.
//
// Thus RX can never gate or reset the TX-derived playback clock. This follows
// the vendor driver's start/timestamp ordering documented in
// documentation/MAUDIO_1814_START_CHOREOGRAPHY.md sections 2 and 4, and Linux's
// special-M-Audio post-domain rate command in
// references/linux-sound-firewire-stack/firewire/bebob/bebob_stream.c:636-663.

#pragma once

#include <cstdint>
#include <variant>

namespace ASFW::Audio::Families::BeBoB::MAudio {

/// The vendor starts TX now and schedules RX 160 FireWire cycles (20 ms) later.
inline constexpr uint32_t kReceiveStartLeadCycles = 160;

/// The vendor output program fills four segments before it lets TX DMA run.
inline constexpr uint8_t kTransmitPrefillSegments = 4;

/// Warm-up facts from the vendor TX callback: group 2 plants the RX reference;
/// group 3 is the first group eligible to publish a playback clock.
inline constexpr uint32_t kCaptureReferenceGroup = 2;
inline constexpr uint32_t kFirstPlaybackClockGroup = 3;

/// Owned by the caller. Every asynchronous event carries the epoch it was
/// issued for, making stale completion callbacks inert without a restart loop.
struct StartEpoch final {
    uint64_t value{0};

    friend constexpr bool operator==(const StartEpoch&,
                                     const StartEpoch&) = default;
};

/// A TX callback has already correlated this FireWire cycle time with the host
/// monotonic clock. The choreography keeps it opaque: the later adapter owns
/// conversion to the shared HostClockAnchor sample, including its sample frame.
struct TxClockAnchor final {
    uint32_t cycleTime{0};
    uint64_t hostTicks{0};

    friend constexpr bool operator==(const TxClockAnchor&,
                                     const TxClockAnchor&) = default;
};

// --- Nested running lanes ---------------------------------------------------

struct DeviceStartConfirming final {};
struct DeviceStartReady final {};
using DeviceStartState = std::variant<DeviceStartConfirming, DeviceStartReady>;

/// Groups before group 2 are deliberately not enough to publish a playback
/// clock: the vendor first plants a capture reference from TX timing.
struct AwaitingCaptureReference final {};

/// This alternative contains the reference, so a playback clock can only be
/// published after the capture-side reference has been planted.
struct AwaitingPlaybackAnchor final {
    TxClockAnchor captureReference{};
};

/// An active playback timeline is necessarily TX-anchored. `lastAnchor` is
/// kept for diagnostics and makes the invariant visible in the state itself.
struct PlaybackRunning final {
    TxClockAnchor lastAnchor{};
};

using PlaybackState = std::variant<AwaitingCaptureReference,
                                   AwaitingPlaybackAnchor,
                                   PlaybackRunning>;

struct CaptureAwaitingData final {};

/// The 1814 can report a header-only DBS=0/SYT=ffff transition before usable
/// capture data. This is a capture condition, not a duplex failure.
struct CaptureInNoDataTransition final {};

struct CaptureReady final {};
struct CaptureDegraded final {};
using CaptureState = std::variant<CaptureAwaitingData,
                                  CaptureInNoDataTransition,
                                  CaptureReady,
                                  CaptureDegraded>;

// --- Top-level states -------------------------------------------------------

struct Configuring final {
    StartEpoch epoch{};
};

struct Connecting final {
    StartEpoch epoch{};
};

struct ArmingHostTransport final {
    StartEpoch epoch{};

    // TX can begin producing callback timestamps before the asynchronous host
    // start completion reaches the adapter queue. Preserve that evidence here
    // instead of assuming completion always wins the race.
    PlaybackState playback{AwaitingCaptureReference{}};
    CaptureState capture{CaptureAwaitingData{}};
};

/// The host transport is running, while device confirmation, TX warm-up, and
/// capture transition continue independently on the serial adapter queue.
struct RunningTransport final {
    StartEpoch epoch{};
    DeviceStartState deviceStart{DeviceStartConfirming{}};
    PlaybackState playback{AwaitingCaptureReference{}};
    CaptureState capture{CaptureAwaitingData{}};
};

struct Stopped final {
    StartEpoch epoch{};
};

struct Failed final {
    StartEpoch epoch{};
};

using ChoreographyState = std::variant<Configuring,
                                       Connecting,
                                       ArmingHostTransport,
                                       RunningTransport,
                                       Stopped,
                                       Failed>;

// --- Events -----------------------------------------------------------------

struct PreStreamConfigurationSucceeded final {
    StartEpoch epoch{};
};
struct PreStreamConfigurationFailed final {
    StartEpoch epoch{};
};
struct DeviceStreamsConnected final {
    StartEpoch epoch{};
};
struct DeviceStreamsConnectionFailed final {
    StartEpoch epoch{};
};
struct HostTransportArmed final {
    StartEpoch epoch{};
};
struct HostTransportArmFailed final {
    StartEpoch epoch{};
};
struct DeviceStartConfirmed final {
    StartEpoch epoch{};
};
struct DeviceStartRejected final {
    StartEpoch epoch{};
};

/// Emitted once per completed TX segment group by the future adapter. The group
/// counter matches the vendor callback's post-increment counter.
struct TxGroupTimestampObserved final {
    StartEpoch epoch{};
    uint32_t groupCounter{0};
    TxClockAnchor anchor{};
};

/// Semantic receive events, deliberately after the profile-specific packet
/// decoder. This machine never parses CIP and never sees raw packet bytes.
struct RxHeaderOnlyNoDataObserved final {
    StartEpoch epoch{};
};
struct RxBecameUsable final {
    StartEpoch epoch{};
};
struct CaptureStartExpired final {
    StartEpoch epoch{};
};

struct StopRequested final {
    StartEpoch epoch{};
};

/// A generation change invalidates every outstanding route, independent of
/// the event epoch. A new generation creates a fresh machine via Begin().
struct GenerationInvalidated final {};

using ChoreographyEvent = std::variant<PreStreamConfigurationSucceeded,
                                       PreStreamConfigurationFailed,
                                       DeviceStreamsConnected,
                                       DeviceStreamsConnectionFailed,
                                       HostTransportArmed,
                                       HostTransportArmFailed,
                                       DeviceStartConfirmed,
                                       DeviceStartRejected,
                                       TxGroupTimestampObserved,
                                       RxHeaderOnlyNoDataObserved,
                                       RxBecameUsable,
                                       CaptureStartExpired,
                                       StopRequested,
                                       GenerationInvalidated>;

// --- Actions ----------------------------------------------------------------

struct ApplyPreStreamConfiguration final {};
struct ConnectDeviceStreams final {};

/// The executor reads the bus cycle timer once, starts TX at +0, schedules RX
/// at +receiveStartLeadCycles, and ensures the TX ring is prefilled before RUN.
struct StartAsymmetricHostTransport final {
    uint32_t transmitStartLeadCycles{0};
    uint32_t receiveStartLeadCycles{kReceiveStartLeadCycles};
    uint8_t transmitPrefillSegments{kTransmitPrefillSegments};
};

/// Re-assert the special M-Audio rate/signal-format command only after host
/// DMA is live. Linux calls this the action that starts device transmission.
struct ConfirmDeviceStart final {};

struct PlantCaptureReference final {
    TxClockAnchor anchor{};
};
struct PublishPlaybackClock final {
    TxClockAnchor anchor{};
};
struct NoteCaptureNoDataTransition final {};
struct MarkCaptureReady final {};
struct MarkCaptureDegraded final {};

/// No host DMA has started yet; the adapter can report the failed start and
/// invalidate its pending callbacks solely through the epoch.
struct FailStart final {};

/// Host DMA may already be live, so the adapter must quiesce it before it
/// reports failure or completes a stop/generation transition.
struct StopHostTransport final {};
struct Done final {};

using ChoreographyAction = std::variant<ApplyPreStreamConfiguration,
                                        ConnectDeviceStreams,
                                        StartAsymmetricHostTransport,
                                        ConfirmDeviceStart,
                                        PlantCaptureReference,
                                        PublishPlaybackClock,
                                        NoteCaptureNoDataTransition,
                                        MarkCaptureReady,
                                        MarkCaptureDegraded,
                                        FailStart,
                                        StopHostTransport,
                                        Done>;

struct ChoreographyStep final {
    ChoreographyState state;
    ChoreographyAction action;
};

/// Starts one attempt. A new attempt must use a fresh epoch and a fresh machine
/// instead of resurrecting a terminal state.
[[nodiscard]] ChoreographyStep Begin(StartEpoch epoch) noexcept;

/// Total over (state, event). Impossible and stale events hold state and return
/// Done, because callbacks naturally race stop and generation changes.
[[nodiscard]] ChoreographyStep Advance(const ChoreographyState& state,
                                       const ChoreographyEvent& event) noexcept;

[[nodiscard]] const char* StateName(const ChoreographyState& state) noexcept;
[[nodiscard]] const char* PlaybackStateName(const PlaybackState& state) noexcept;
[[nodiscard]] const char* CaptureStateName(const CaptureState& state) noexcept;
[[nodiscard]] const char* DeviceStartStateName(
    const DeviceStartState& state) noexcept;

[[nodiscard]] bool IsTerminal(const ChoreographyState& state) noexcept;
[[nodiscard]] bool IsPlaybackRunning(const ChoreographyState& state) noexcept;

} // namespace ASFW::Audio::Families::BeBoB::MAudio
