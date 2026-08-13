// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Executes only the synchronous bring-up edge of MAudioDuplexChoreography.
// The generic duplex coordinator remains responsible for resource ownership,
// FCP/CMP calls, rollback, and teardown. This adapter owns the one M-Audio
// difference that is observable to the transport: start prefilled TX now, then
// arm RX on the same sampled bus timer + 160 cycles.

#pragma once

#include "MAudioDuplexChoreography.hpp"
#include "../../../Duplex/IsochDuplexHostTransport.hpp"

#include <cstdint>

namespace ASFW::Audio::Families::BeBoB::MAudio {

struct HostTransportArmResult final {
    kern_return_t status{kIOReturnSuccess};
    bool transmitStarted{false};
    bool receiveStarted{false};
    uint32_t receiveStartCycleTimer{0};
};

// This is intentionally a small, serial adapter rather than a second generic
// duplex coordinator. Its state is the variant-based choreography machine;
// all host transport calls stay behind the existing payload-opaque seam.
class DuplexStartAdapter final {
  public:
    DuplexStartAdapter(IIsochDuplexHostTransport& hostTransport,
                       Driver::HardwareInterface& hardware) noexcept
        : hostTransport_(hostTransport), hardware_(hardware) {}

    void Begin(StartEpoch epoch) noexcept;
    [[nodiscard]] kern_return_t OnPreStreamConfigurationSucceeded() noexcept;
    void OnPreStreamConfigurationFailed() noexcept;

    // Call only after ProgramRx + ProgramTxAndEnableDuplex succeeded. It
    // consumes StartAsymmetricHostTransport from the pure machine and executes
    // TX-now / RX-future at the transport boundary.
    [[nodiscard]] HostTransportArmResult
    OnDeviceStreamsConnectedAndArmHost() noexcept;
    void OnDeviceStreamsConnectionFailed() noexcept;

    [[nodiscard]] kern_return_t OnDeviceStartConfirmed() noexcept;
    void OnDeviceStartRejected() noexcept;

    [[nodiscard]] const ChoreographyState& State() const noexcept { return state_; }
    [[nodiscard]] StartEpoch Epoch() const noexcept { return epoch_; }

  private:
    [[nodiscard]] ChoreographyStep Advance(ChoreographyEvent event) noexcept;
    [[nodiscard]] kern_return_t UnexpectedAction(const char* stage) const noexcept;

    IIsochDuplexHostTransport& hostTransport_;
    Driver::HardwareInterface& hardware_;
    StartEpoch epoch_{};
    ChoreographyState state_{Stopped{}};
};

} // namespace ASFW::Audio::Families::BeBoB::MAudio
