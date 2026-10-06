// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FamilyDriver.hpp - What the session asks of a device family.
//
// documentation/AUDIO_SESSION_REDESIGN.md §4.2. A pure interface: only pure
// virtuals and a virtual destructor, so every family states its answer to every
// step. A step with nothing to do is written out in the family, with its
// reason; it is never inherited. Shared sequencing lives in Audio/Session's
// RestartRoutine and StopRoutine, never here. Each device protocol implements
// it (IDeviceProtocol::AsFamilyDriver); the session consumes it.
//
// The stages reproduce the wire order recorded in S2: device RX is armed
// before device TX, and TX arming and the device enable are one step because
// DICE writes GLOBAL_ENABLE straight after the last TX stream.
//
// Threading: every method blocks until the device answers. The session calls
// them on its caller's thread, never on the driver's Default queue, where the
// bus completions they wait for are delivered.

#pragma once
#include <span>

#include "DuplexControlTypes.hpp"
#include "../../Runtime/ResolvedAudioConfiguration.hpp"

#include <DriverKit/IOReturn.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>
#include <array>

namespace ASFW::Audio {

class FamilyDriver {
public:
    struct DirectionResourcePolicy {
        bool reserveHostResources{true};
        uint64_t allowedIsoChannels{~uint64_t{0}};
    };
    struct ResourcePolicy {
        DirectionResourcePolicy playback{};
        DirectionResourcePolicy capture{};
    };
    struct StopPolicy {
        bool stopHostContextsBeforeDevice{false};
    };
    virtual ~FamilyDriver() = default;

    // Service teardown: once `cancel` reads true, waits give up and no new
    // device work starts. Null clears it.
    virtual void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept = 0;

    // Geometry discovery derived from descriptors or plug formats, offered
    // before LoadGeometry. A family that reads its own geometry ignores it.
    virtual void AdoptDiscoveredRates(std::span<const uint32_t>) {}
    // Complete candidates are separate from advertised/validated rates. The
    // session offers them without granting permission to program a new rate.
    virtual void AdoptDiscoveredFormations(std::span<const Runtime::RateFormation>) {}
    virtual void AdoptDiscoveredGeometry(const AudioStreamRuntimeCaps& caps) noexcept { (void)caps; }
    // Read the device's stream geometry so channel planning sees every stream.
    [[nodiscard]] virtual IOReturn LoadGeometry() = 0;
    // The geometry last read from the device, if any.
    [[nodiscard]] virtual std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const = 0;

    // Claim the device and bring its clock to `clock`.
    [[nodiscard]] virtual std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) = 0;
    // Apply channel assignment and return the final channels. A device-selected
    // direction can resolve its channel here before host DMA is prepared.
    [[nodiscard]] virtual std::expected<AudioDuplexChannels, IOReturn> AssignChannels(
        const AudioDuplexChannels& channels) = 0;
    [[nodiscard]] virtual ResourcePolicy GetResourcePolicy() const noexcept { return {}; }
    [[nodiscard]] virtual StopPolicy GetStopPolicy() const noexcept { return {}; }
    // Families with device settle time after their enable command may raise
    // the shared session delay; absent preserves the resolved profile value.
    [[nodiscard]] virtual std::optional<uint32_t> PostEnableDelayMs() const noexcept { return std::nullopt; }
    // Clock and lock state, for the pre-stream clock gate.
    [[nodiscard]] virtual std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) = 0;
    // Arm the streams the device receives (host playback).
    [[nodiscard]] virtual std::expected<DuplexStageResult, IOReturn> ArmDeviceRx() = 0;
    // Arm the streams the device transmits (host capture) and enable streaming.
    [[nodiscard]] virtual std::expected<DuplexStageResult, IOReturn> ArmDeviceTxAndEnable() = 0;
    // Confirm the device runs what was armed.
    [[nodiscard]] virtual std::expected<DuplexConfirmResult, IOReturn> Confirm() = 0;

    // Change the clock while no stream runs.
    [[nodiscard]] virtual std::expected<DuplexClockApplyResult, IOReturn> ApplyClockIdle(
        const AudioClockConfig& clock) = 0;

    // Staged stop, for a recipe that interleaves device disconnects with host stops.
    [[nodiscard]] virtual IOReturn DisconnectPlayback() = 0;
    [[nodiscard]] virtual IOReturn DisconnectCapture() = 0;
    // Drop every device connection before a rollback stop.
    [[nodiscard]] virtual IOReturn BreakConnections() = 0;
    // Stop everything on the device side, whatever state it is in.
    [[nodiscard]] virtual IOReturn Stop() = 0;
};

} // namespace ASFW::Audio
