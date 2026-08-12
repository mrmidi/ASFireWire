// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "AVCUnit.hpp"
#include "Probe/AVCPlug0StreamDiscovery.hpp"

#include <functional>
#include <memory>

namespace ASFW::Protocols::AVC {

// Standards-only, unit-scoped AV/C session. Family adapters compose this
// object; it has no dependency on Audio or any vendor/model catalog.
class RemoteAvcSession final {
public:
    explicit RemoteAvcSession(std::shared_ptr<AVCUnit> unit) noexcept
        : unit_(std::move(unit)) {}

    [[nodiscard]] Discovery::UnitInstanceId UnitId() const noexcept;
    [[nodiscard]] Discovery::DeviceInstanceId DeviceId() const noexcept;
    [[nodiscard]] uint64_t ObservedGuid() const noexcept;
    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] std::shared_ptr<FCPTransport> Transport() const noexcept;
    [[nodiscard]] AVCUnit* Unit() const noexcept { return unit_.get(); }

    void Initialize(std::function<void(bool)> completion);
    void ProbePlug0(Probe::ReadOnlyProbeCompletion completion);
    [[nodiscard]] bool HasAudioSubunit() const noexcept;
    [[nodiscard]] bool HasMusicSubunit() const noexcept;

private:
    std::shared_ptr<AVCUnit> unit_;
};

} // namespace ASFW::Protocols::AVC
