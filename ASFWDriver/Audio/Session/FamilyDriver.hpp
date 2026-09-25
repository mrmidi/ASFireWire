// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FamilyDriver.hpp - What the session asks of a device family.
//
// documentation/AUDIO_SESSION_REDESIGN.md §4.2. A pure interface: only pure
// virtuals and a virtual destructor, so every family states its answer to every
// step. A step with nothing to do is written out in the family, with its
// reason; it is never inherited. Shared sequencing lives in RestartRoutine and
// StopRoutine, never here.
//
// The stages are shaped (stage S2) to reproduce today's wire order exactly:
// device RX is armed before device TX, and TX arming and the device enable are
// one step because DICE writes GLOBAL_ENABLE straight after the last TX stream.
// Stage S5 reshapes this when the families get native drivers.
//
// Threading: every method blocks until the device answers. The session calls
// them on its caller's thread, never on the driver's Default queue, where the
// bus completions they wait for are delivered.

#pragma once

#include "../Protocols/Duplex/DuplexControlTypes.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>
#include <expected>
#include <optional>

namespace ASFW::Audio::Session {

class FamilyDriver {
public:
    virtual ~FamilyDriver() = default;

    // Read the device's stream geometry so channel planning sees every stream.
    [[nodiscard]] virtual IOReturn LoadGeometry() = 0;
    // The geometry last read from the device, if any.
    [[nodiscard]] virtual std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const = 0;

    // Claim the device and bring its clock to `clock`.
    [[nodiscard]] virtual std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) = 0;
    // The channels the IRM assigned; the family writes them to the device.
    virtual void AssignChannels(const AudioDuplexChannels& channels) = 0;
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

} // namespace ASFW::Audio::Session
