// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "OrpheusTypes.hpp"
#include "OrpheusVendorCodec.hpp"
#include "OrpheusControlModel.hpp"
#include "OrpheusControlWrite.hpp"
#include "../../GenericAvcProtocol.hpp"
#include "../../../Controls/IControlProvider.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ASFW::Audio::Prism::Orpheus {

class OrpheusProtocol : public GenericAvcProtocol,
                        public ASFW::Audio::Controls::IControlProvider {
public:
    using VoidCallback = std::function<void(IOReturn)>;
    template<typename T> using ResultCallback = std::function<void(IOReturn, T)>;

    OrpheusProtocol(Protocols::Ports::FireWireBusOps& busOps,
                    Protocols::Ports::FireWireBusInfo& busInfo,
                    Discovery::DeviceRouteToken route,
                    IRM::IRMClient* irmClient = nullptr,
                    CMP::CMPClient* cmpClient = nullptr,
                    Scheduling::ITimerScheduler* timerScheduler = nullptr) noexcept;
    ~OrpheusProtocol() override;

    // IDeviceProtocol overrides
    [[nodiscard]] const char* GetName() const override { return "Prism Sound Orpheus"; }
    [[nodiscard]] bool HasDsp() const override { return true; }
    [[nodiscard]] bool HasMixer() const override { return true; }
    Controls::IControlProvider* Controls() noexcept override { return this; }

    IOReturn Shutdown() override;
    void UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                              std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) override;

    // IControlProvider implementation
    void Prepare(std::function<void(IOReturn)> done) override;
    std::optional<ASFW::Audio::Controls::ControlSnapshot> CopySnapshot() const override;
    void SetParameter(AudioModel::ParameterId id, const AudioModel::ParameterValue& value, Completion done) override;
    void SetRoute(AudioModel::NodeId router, AudioModel::PortId output, std::optional<AudioModel::PortId> input, Completion done) override;

    // Frame transmission helpers
    void SendOrpheusFrame(std::span<const uint8_t> frameBytes,
                          std::function<void(IOReturn, std::vector<uint8_t>)> callback);

    void ExecuteOrpheusSequence(const std::vector<std::vector<uint8_t>>& frames,
                                std::function<void(IOReturn, std::vector<std::vector<uint8_t>>)> callback);

    [[nodiscard]] const OrpheusControlsState& State() const noexcept { return controls_; }

private:
    void ReadControlGroup(OrpheusControlGroup group, uint8_t index, VoidCallback done);
    void FinishControlWrite(AudioModel::ParameterId id, Completion done);
    bool CommitControls();

    void QueueControl(std::function<void(OrpheusProtocol&)> op, Completion refuse);
    void ReleaseControl();
    void ResetControlQueue();

    static constexpr size_t kMaxControlWaiters = 16;
    std::deque<std::function<void()>> controlWaiters_;
    bool controlsReady_{false};
    bool controlBusy_{false};
    uint64_t controlsEpoch_{0};

    OrpheusControlsState controls_{};
    std::optional<ASFW::Audio::Controls::ControlSnapshot> controlSnapshot_;
};

} // namespace ASFW::Audio::Prism::Orpheus
