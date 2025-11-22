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

/**
 * @brief Observer interface for device lifecycle events.
 *
 * Clients implement this to receive notifications when devices
 * are added, resumed, suspended, or removed from the bus.
 */
class IDeviceObserver {
public:
    virtual ~IDeviceObserver() = default;

    /**
     * @brief Called when new device is discovered and published.
     * @param device Newly published device
     */
    virtual void OnDeviceAdded(std::shared_ptr<FWDevice> device) = 0;

    /**
     * @brief Called when device reappears after bus reset.
     * @param device Resumed device (generation/nodeId updated)
     */
    virtual void OnDeviceResumed(std::shared_ptr<FWDevice> device) = 0;

    /**
     * @brief Called when device lost after bus reset.
     * @param device Suspended device (still in registry, but not present)
     */
    virtual void OnDeviceSuspended(std::shared_ptr<FWDevice> device) = 0;

    /**
     * @brief Called when device permanently removed.
     * @param guid GUID of removed device
     */
    virtual void OnDeviceRemoved(Guid64 guid) = 0;
};

/**
 * @brief Observer interface for unit lifecycle events.
 *
 * Clients implement this to receive notifications when units
 * are published, suspended, resumed, or terminated.
 */
class IUnitObserver {
public:
    virtual ~IUnitObserver() = default;

    /**
     * @brief Called when unit is published and ready for use.
     * @param unit Newly published unit
     */
    virtual void OnUnitPublished(std::shared_ptr<FWUnit> unit) = 0;

    /**
     * @brief Called when unit suspended (parent device lost).
     * @param unit Suspended unit
     */
    virtual void OnUnitSuspended(std::shared_ptr<FWUnit> unit) = 0;

    /**
     * @brief Called when unit resumed (parent device reappeared).
     * @param unit Resumed unit
     */
    virtual void OnUnitResumed(std::shared_ptr<FWUnit> unit) = 0;

    /**
     * @brief Called when unit permanently terminated.
     * @param unit Terminated unit (last chance to access)
     */
    virtual void OnUnitTerminated(std::shared_ptr<FWUnit> unit) = 0;
};

/**
 * @brief Unit registry interface for spec-based unit discovery.
 *
 * Analogous to IOKit's matching and notification system. Clients use this
 * to find units matching specific spec IDs and receive notifications when
 * matching units appear.
 */
class IUnitRegistry {
public:
    virtual ~IUnitRegistry() = default;

    /**
     * @brief Find all units matching spec ID across all devices.
     *
     * @param specId Required Unit_Spec_ID (e.g., 0x00A02D for 1394 TA Audio)
     * @param swVersion Optional Unit_SW_Version (matches any if not provided)
     * @return Vector of matching units (includes Ready and Suspended units)
     */
    virtual std::vector<std::shared_ptr<FWUnit>> FindUnitsBySpec(
        uint32_t specId,
        std::optional<uint32_t> swVersion = {}
    ) const = 0;

    /**
     * @brief Get all units across all devices.
     * @return Vector of all units (includes Ready and Suspended units)
     */
    virtual std::vector<std::shared_ptr<FWUnit>> GetAllUnits() const = 0;

    /**
     * @brief Get all Ready units (excludes Suspended/Terminated).
     * @return Vector of ready units
     */
    virtual std::vector<std::shared_ptr<FWUnit>> GetReadyUnits() const = 0;

    /**
     * @brief Register observer for unit lifecycle events.
     *
     * Observer will receive callbacks for all unit lifecycle events.
     * Observer must remain valid until unregistered.
     *
     * @param observer Observer to register (non-null)
     */
    virtual void RegisterUnitObserver(IUnitObserver* observer) = 0;

    /**
     * @brief Unregister observer for unit lifecycle events.
     * @param observer Observer to unregister
     */
    virtual void UnregisterUnitObserver(IUnitObserver* observer) = 0;

    // Callback types for unit monitoring
    using UnitCallback = std::function<void(std::shared_ptr<FWUnit>)>;
    using CallbackHandle = uint64_t;

    /**
     * @brief Register callback for when matching unit appears.
     *
     * Callback invoked immediately for any existing matching units,
     * and again whenever new matching unit is published.
     *
     * @param specId Required Unit_Spec_ID
     * @param swVersion Optional Unit_SW_Version
     * @param callback Callback to invoke (captured by value)
     * @return Callback handle (dispose to unregister)
     */
    virtual CallbackHandle RegisterUnitCallback(
        uint32_t specId,
        std::optional<uint32_t> swVersion,
        UnitCallback callback
    ) = 0;

    /**
     * @brief Unregister callback by handle.
     * @param handle Handle returned by RegisterUnitCallback
     */
    virtual void UnregisterCallback(CallbackHandle handle) = 0;
};

/**
 * @brief Device manager interface combining device and unit management.
 *
 * Central registry for all FireWire devices and units. Maintains device
 * identity across bus resets via GUID-based tracking.
 */
class IDeviceManager : public IUnitRegistry {
public:
    virtual ~IDeviceManager() = default;

    /**
     * @brief Get device by GUID (stable across bus resets).
     * @param guid Device GUID
     * @return Device pointer, or nullptr if not found
     */
    virtual std::shared_ptr<FWDevice> GetDeviceByGUID(Guid64 guid) const = 0;

    /**
     * @brief Get device by current generation and node ID.
     * @param gen Generation number
     * @param nodeId Node ID
     * @return Device pointer, or nullptr if not found
     */
    virtual std::shared_ptr<FWDevice> GetDeviceByNode(
        Generation gen,
        uint8_t nodeId
    ) const = 0;

    /**
     * @brief Get all devices in current generation.
     * @param gen Generation number
     * @return Vector of devices present in generation (includes Suspended)
     */
    virtual std::vector<std::shared_ptr<FWDevice>> GetDevicesByGeneration(
        Generation gen
    ) const = 0;

    /**
     * @brief Get all devices (across all generations).
     * @return Vector of all devices in registry
     */
    virtual std::vector<std::shared_ptr<FWDevice>> GetAllDevices() const = 0;

    /**
     * @brief Get all Ready devices (excludes Suspended/Terminated).
     * @return Vector of ready devices
     */
    virtual std::vector<std::shared_ptr<FWDevice>> GetReadyDevices() const = 0;

    /**
     * @brief Register observer for device lifecycle events.
     *
     * Observer will receive callbacks for all device lifecycle events.
     * Observer must remain valid until unregistered.
     *
     * @param observer Observer to register (non-null)
     */
    virtual void RegisterDeviceObserver(IDeviceObserver* observer) = 0;

    /**
     * @brief Unregister observer for device lifecycle events.
     * @param observer Observer to unregister
     */
    virtual void UnregisterDeviceObserver(IDeviceObserver* observer) = 0;

    // === Internal API (called by ControllerCore discovery) ===

    /**
     * @brief Add or update device from discovery scan.
     *
     * Called by ControllerCore when ROM parsing completes. Creates FWDevice
     * from DeviceRecord and ConfigROM, or updates existing device if GUID
     * already registered.
     *
     * @param record Device record from DeviceRegistry
     * @param rom Parsed Config ROM
     * @return Device (new or existing)
     */
    virtual std::shared_ptr<FWDevice> UpsertDevice(
        const DeviceRecord& record,
        const ConfigROM& rom
    ) = 0;

    /**
     * @brief Mark device as lost (not present in current generation).
     *
     * Called by ControllerCore during bus reset when device doesn't reappear.
     * Device transitions to Suspended state but remains in registry.
     *
     * @param guid Device GUID
     */
    virtual void MarkDeviceLost(Guid64 guid) = 0;

    /**
     * @brief Terminate device (permanent removal).
     *
     * Called by ControllerCore when device removed from bus or driver stopping.
     * Device transitions to Terminated state and removed from registry.
     *
     * @param guid Device GUID
     */
    virtual void TerminateDevice(Guid64 guid) = 0;
};

/**
 * @brief RAII guard for automatic observer unregistration.
 *
 * Example:
 * @code
 * class MyClient : public IUnitObserver {
 *     ObserverGuard<IUnitObserver> guard_;
 * public:
 *     void Start(IUnitRegistry& registry) {
 *         guard_ = ObserverGuard<IUnitObserver>(registry, this);
 *     }
 * };
 * @endcode
 */
template<typename ObserverType>
class ObserverGuard {
public:
    ObserverGuard() = default;

    // Register observer on construction
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

    // Unregister on destruction
    ~ObserverGuard() noexcept {
        if (unregister_) {
            unregister_();
        }
    }

    // Move-only (no copy)
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
            // Unregister current
            if (unregister_) {
                unregister_();
            }
            // Move from other
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
