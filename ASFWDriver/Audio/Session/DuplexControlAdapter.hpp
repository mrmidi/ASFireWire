// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexControlAdapter.hpp - A FamilyDriver over an existing IDuplexDeviceControl.
//
// Stage S2 runs every family through this adapter; stage S5 replaces it with
// native family drivers. It is the one place that turns the callback-style
// device stages into blocking calls.

#pragma once

#include "../Protocols/Duplex/FamilyDriver.hpp"

#include "../Protocols/Duplex/IDuplexDeviceControl.hpp"
#include "../Protocols/IDeviceProtocol.hpp"

#include <atomic>
#include <memory>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio::Session {

class DuplexControlAdapter final : public FamilyDriver {
public:
    // Stage waits give up after this long; the device stages bound their own
    // waits well inside it.
    static constexpr uint32_t kStageTimeoutMs = 12000;

    // Holds the protocol for as long as the adapter lives. `cancel` is the
    // service teardown token; every wait checks it.
    DuplexControlAdapter(std::shared_ptr<IDeviceProtocol> protocol,
                         IDuplexDeviceControl& control,
                         const std::atomic<bool>* cancel) noexcept;

    // The IRM client the protocol was built with. A bus resource the session
    // hands to the restart routine; it is not part of FamilyDriver (§4.2 rule 5).
    [[nodiscard]] ::ASFW::IRM::IRMClient* IrmClient() const noexcept;
    void SetTeardownCancelToken(const std::atomic<bool>* cancel) noexcept override {
        control_.SetTeardownCancelToken(cancel);
    }

    [[nodiscard]] IOReturn LoadGeometry() override;
    [[nodiscard]] std::optional<AudioStreamRuntimeCaps> RuntimeCaps() const override;
    [[nodiscard]] std::expected<DuplexPrepareResult, IOReturn> Configure(
        const AudioDuplexChannels& channels, const AudioClockConfig& clock) override;
    void AssignChannels(const AudioDuplexChannels& channels) override;
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceRx() override;
    [[nodiscard]] std::expected<DuplexStageResult, IOReturn> ArmDeviceTxAndEnable() override;
    [[nodiscard]] std::expected<DuplexConfirmResult, IOReturn> Confirm() override;
    [[nodiscard]] std::expected<DuplexClockApplyResult, IOReturn> ApplyClockIdle(
        const AudioClockConfig& clock) override;
    [[nodiscard]] IOReturn DisconnectPlayback() override;
    [[nodiscard]] IOReturn DisconnectCapture() override;
    [[nodiscard]] IOReturn BreakConnections() override;
    [[nodiscard]] IOReturn Stop() override;

private:
    std::shared_ptr<IDeviceProtocol> protocol_;
    IDuplexDeviceControl& control_;
    const std::atomic<bool>* cancel_;
};

} // namespace ASFW::Audio::Session
