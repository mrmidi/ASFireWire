#pragma once

#include <memory>
#include <functional>
#include <vector>
#include <optional>
#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Forward declarations
class FWDevice;
class FWUnit;

class IDeviceObserver {
public:
    virtual ~IDeviceObserver() = default;

    virtual void OnDeviceAdded(std::shared_ptr<FWDevice> device) = 0;
    virtual void OnDeviceResumed(std::shared_ptr<FWDevice> device) = 0;
    virtual void OnDeviceSuspended(std::shared_ptr<FWDevice> device) = 0;
    virtual void OnDeviceRemoved(Guid64 guid) = 0;
};

class IUnitObserver {
public:
    virtual ~IUnitObserver() = default;

    virtual void OnUnitPublished(std::shared_ptr<FWUnit> unit) = 0;
    virtual void OnUnitSuspended(std::shared_ptr<FWUnit> unit) = 0;
    virtual void OnUnitResumed(std::shared_ptr<FWUnit> unit) = 0;
    virtual void OnUnitTerminated(std::shared_ptr<FWUnit> unit) = 0;
};

class IUnitRegistry {
public:
    virtual ~IUnitRegistry() = default;

    virtual std::vector<std::shared_ptr<FWUnit>> FindUnitsBySpec(
        uint32_t specId,
        std::optional<uint32_t> swVersion = {}
    ) const = 0;

    virtual std::vector<std::shared_ptr<FWUnit>> GetAllUnits() const = 0;

    virtual std::vector<std::shared_ptr<FWUnit>> GetReadyUnits() const = 0;

    virtual void RegisterUnitObserver(IUnitObserver* observer) = 0;

    virtual void UnregisterUnitObserver(IUnitObserver* observer) = 0;

    using UnitCallback = std::function<void(std::shared_ptr<FWUnit>)>;
    using CallbackHandle = uint64_t;

    virtual CallbackHandle RegisterUnitCallback(
        uint32_t specId,
        std::optional<uint32_t> swVersion,
        UnitCallback callback
    ) = 0;

    virtual void UnregisterCallback(CallbackHandle handle) = 0;
};

class IDeviceManager : public IUnitRegistry {
public:
    virtual ~IDeviceManager() = default;

    virtual std::shared_ptr<FWDevice> GetDeviceByGUID(Guid64 guid) const = 0;

    virtual std::shared_ptr<FWDevice> GetDeviceByNode(
        Generation gen,
        uint8_t nodeId
    ) const = 0;

    virtual std::vector<std::shared_ptr<FWDevice>> GetDevicesByGeneration(
        Generation gen
    ) const = 0;

    virtual std::vector<std::shared_ptr<FWDevice>> GetAllDevices() const = 0;

    virtual std::vector<std::shared_ptr<FWDevice>> GetReadyDevices() const = 0;

    virtual void RegisterDeviceObserver(IDeviceObserver* observer) = 0;

    virtual void UnregisterDeviceObserver(IDeviceObserver* observer) = 0;

    virtual std::shared_ptr<FWDevice> UpsertDevice(
        const DeviceRecord& record,
        const ConfigROM& rom
    ) = 0;

    virtual void MarkDeviceLost(Guid64 guid) = 0;

    virtual void TerminateDevice(Guid64 guid) = 0;
};

template<typename ObserverType>
class ObserverGuard {
public:
    ObserverGuard() = default;

    template<typename RegistryType>
    ObserverGuard(RegistryType& registry, ObserverType* observer)
        : observer_(observer)
    {
        if constexpr (std::is_same_v<ObserverType, IDeviceObserver>) {
            auto* mgr = dynamic_cast<IDeviceManager*>(&registry);
            if (mgr) {
                unregister_ = [mgr, observer]() {
                    mgr->UnregisterDeviceObserver(observer);
                };
                mgr->RegisterDeviceObserver(observer);
            }
        } else if constexpr (std::is_same_v<ObserverType, IUnitObserver>) {
            unregister_ = [&registry, observer]() {
                registry.UnregisterUnitObserver(observer);
            };
            registry.RegisterUnitObserver(observer);
        }
    }

    ~ObserverGuard() noexcept {
        if (unregister_) {
            unregister_();
        }
    }

    ObserverGuard(const ObserverGuard&) = delete;
    ObserverGuard& operator=(const ObserverGuard&) = delete;

    ObserverGuard(ObserverGuard&& other) noexcept
        : observer_(other.observer_)
        , unregister_(std::move(other.unregister_))
    {
        other.observer_ = nullptr;
        other.unregister_ = nullptr;
    }

    ObserverGuard& operator=(ObserverGuard&& other) noexcept {
        if (this != &other) {
            if (unregister_) {
                unregister_();
            }
            observer_ = other.observer_;
            unregister_ = std::move(other.unregister_);
            other.observer_ = nullptr;
            other.unregister_ = nullptr;
        }
        return *this;
    }

private:
    ObserverType* observer_{nullptr};
    std::function<void()> unregister_;
};

} // namespace ASFW::Discovery
