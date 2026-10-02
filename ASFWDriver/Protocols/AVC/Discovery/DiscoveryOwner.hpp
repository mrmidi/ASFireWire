// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../AVCUnit.hpp"
namespace ASFW::Protocols::AVC {
/// Composition seam for device policy/preparation and consumers of discovered
/// facts. Generic AV/C has no endpoint, startup-control or device-family rules.
class DiscoveryOwner {
public:
    virtual ~DiscoveryOwner() = default;
    virtual void PrepareProducer(std::shared_ptr<Discovery::FWUnit> unit, std::function<void(bool)> ready) = 0;
    [[nodiscard]] virtual bool AllowsDiscovery(const Discovery::FWUnit& unit) const = 0;
    /// Manual diagnostics (refresh) may run now: the unit allows discovery and
    /// its audio is idle. Never send diagnostics to a streaming device.
    [[nodiscard]] virtual bool AllowsManualDiagnostics(const Discovery::FWUnit& unit) const = 0;
    [[nodiscard]] virtual AVCUnit::DiscoveryOptions OptionsFor(const Discovery::FWUnit& unit) const = 0;
    virtual void UnitCreated(const std::shared_ptr<AVCUnit>& unit) = 0;
    virtual void UnitCompleted(const std::shared_ptr<AVCUnit>& unit, bool success) = 0;
    virtual void DeviceAdded(std::shared_ptr<Discovery::FWDevice> device) = 0;
    virtual void Shutdown() = 0;
};
}
