#pragma once

#include "IDeviceManager.hpp"
#include "FWDevice.hpp"
#include "FWUnit.hpp"
#include <map>
#include <vector>
#include <set>
#include <atomic>
#include <DriverKit/IOLib.h>

namespace ASFW::Discovery {

class DeviceManager : public IDeviceManager {
public:
    DeviceManager();
    ~DeviceManager() override;

    std::vector<std::shared_ptr<FWUnit>> FindUnitsBySpec(
        uint32_t specId,
        std::optional<uint32_t> swVersion = {}
    ) const override;

    std::vector<std::shared_ptr<FWUnit>> GetAllUnits() const override;

    std::vector<std::shared_ptr<FWUnit>> GetReadyUnits() const override;

    void RegisterUnitObserver(IUnitObserver* observer) override;

    void UnregisterUnitObserver(IUnitObserver* observer) override;

    CallbackHandle RegisterUnitCallback(
        uint32_t specId,
        std::optional<uint32_t> swVersion,
        UnitCallback callback
    ) override;

    void UnregisterCallback(CallbackHandle handle) override;

    // === IDeviceManager Implementation ===

    std::shared_ptr<FWDevice> GetDeviceByGUID(Guid64 guid) const override;

    std::shared_ptr<FWDevice> GetDeviceByNode(
        Generation gen,
        uint8_t nodeId
    ) const override;

    std::vector<std::shared_ptr<FWDevice>> GetDevicesByGeneration(
        Generation gen
    ) const override;

    std::vector<std::shared_ptr<FWDevice>> GetAllDevices() const override;

    std::vector<std::shared_ptr<FWDevice>> GetReadyDevices() const override;

    void RegisterDeviceObserver(IDeviceObserver* observer) override;

    void UnregisterDeviceObserver(IDeviceObserver* observer) override;

    // === Internal API ===

    std::shared_ptr<FWDevice> UpsertDevice(
        const DeviceRecord& record,
        const ConfigROM& rom
    ) override;

    void MarkDeviceLost(Guid64 guid) override;

    void TerminateDevice(Guid64 guid) override;

private:
    void NotifyDeviceAdded(std::shared_ptr<FWDevice> device);
    void NotifyDeviceResumed(std::shared_ptr<FWDevice> device);
    void NotifyDeviceSuspended(std::shared_ptr<FWDevice> device);
    void NotifyDeviceRemoved(Guid64 guid);

    void NotifyUnitPublished(std::shared_ptr<FWUnit> unit);
    void NotifyUnitSuspended(std::shared_ptr<FWUnit> unit);
    void NotifyUnitResumed(std::shared_ptr<FWUnit> unit);
    void NotifyUnitTerminated(std::shared_ptr<FWUnit> unit);

    bool UnitMatchesCallback(
        const std::shared_ptr<FWUnit>& unit,
        uint32_t specId,
        std::optional<uint32_t> swVersion
    ) const;

    mutable IOLock* mutex_;
    std::map<Guid64, std::shared_ptr<FWDevice>> devicesByGuid_;

    using GenNodeKey = uint32_t;
    static GenNodeKey MakeKey(Generation gen, uint8_t nodeId);
    std::map<GenNodeKey, Guid64> genNodeToGuid_;

    std::set<IDeviceObserver*> deviceObservers_;
    std::set<IUnitObserver*> unitObservers_;

    struct UnitCallbackEntry {
        CallbackHandle handle;
        uint32_t specId;
        std::optional<uint32_t> swVersion;
        UnitCallback callback;
    };
    std::vector<UnitCallbackEntry> unitCallbacks_;
    std::atomic<CallbackHandle> nextCallbackHandle_{1};
};

} // namespace ASFW::Discovery
