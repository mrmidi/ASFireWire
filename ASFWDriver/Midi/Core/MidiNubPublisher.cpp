//
// MidiNubPublisher.cpp
// ASFWDriver
//

#include "MidiNubPublisher.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSSharedPtr.h>

#include <net.asfw.driver/ASFWMidiNub.h>

#include "../../Logging/Logging.hpp"
#include "MidiNubProperties.hpp"

namespace ASFW::Midi {

namespace {

namespace Keys = NubKeys;

void SetNumber(OSDictionary* properties, const char* key, uint64_t value, uint32_t bits) {
    auto number = OSSharedPtr(OSNumber::withNumber(value, bits), OSNoRetain);
    if (number) properties->setObject(key, number.get());
}

void SetString(OSDictionary* properties, const char* key, const char* value) {
    if (value == nullptr) return;
    auto text = OSSharedPtr(OSString::withCString(value), OSNoRetain);
    if (text) properties->setObject(key, text.get());
}

void SetBool(OSDictionary* properties, const char* key, bool value) {
    // DriverKit's OSBoolean has no withBoolean; the singletons are the only way
    // to make one, and they must not be released.
    OSBoolean* flag = value ? kOSBooleanTrue : kOSBooleanFalse;
    properties->setObject(key, flag);
}

} // namespace

MidiNubPublisher::MidiNubPublisher(IOService* driver) noexcept : driver_(driver) {
    lock_ = IOLockAlloc();
    if (lock_ == nullptr) {
        ASFW_LOG_ERROR(Midi, "MidiNubPublisher: failed to allocate lock");
    }
}

MidiNubPublisher::~MidiNubPublisher() noexcept {
    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

bool MidiNubPublisher::HasNub(uint64_t endpointId) const noexcept {
    if (lock_ == nullptr) return false;
    IOLockLock(lock_);
    bool found = false;
    for (const auto& entry : entries_) {
        if (entry.used && entry.endpointId == endpointId) { found = true; break; }
    }
    IOLockUnlock(lock_);
    return found;
}

bool MidiNubPublisher::EnsureNub(const MidiEndpointCapabilities& caps,
                                 const uint64_t endpointId,
                                 const char* deviceName,
                                 const char* model,
                                 const char* manufacturer) noexcept {
    if (driver_ == nullptr || lock_ == nullptr || endpointId == 0) return false;

    if (!caps.AnyUsable()) {
        // Normal for most FireWire audio interfaces. Say why once, at notice
        // level, so a device that should have MIDI and does not is visible
        // without turning on debug logging.
        ASFW_LOG(Midi,
                 "MidiNubPublisher: endpoint=%llx publishes no MIDI "
                 "(deviceToHost=%{public}s hostToDevice=%{public}s)",
                 endpointId,
                 MidiCapabilityStatusName(caps.deviceToHost.status),
                 MidiCapabilityStatusName(caps.hostToDevice.status));
        return true;
    }

    // Reserve the slot under the lock so two publications cannot both create.
    IOLockLock(lock_);
    Entry* slot = nullptr;
    for (auto& entry : entries_) {
        if (entry.used && entry.endpointId == endpointId) {
            IOLockUnlock(lock_);
            return true;
        }
        if (slot == nullptr && !entry.used) slot = &entry;
    }
    if (slot == nullptr) {
        IOLockUnlock(lock_);
        ASFW_LOG_ERROR(Midi, "MidiNubPublisher: no free slot for endpoint=%llx", endpointId);
        return false;
    }
    slot->used = true;
    slot->endpointId = endpointId;
    slot->nub = nullptr;
    IOLockUnlock(lock_);

    const auto abandon = [&]() noexcept {
        IOLockLock(lock_);
        slot->used = false;
        slot->endpointId = 0;
        slot->nub = nullptr;
        IOLockUnlock(lock_);
    };

    IOService* nubService = nullptr;
    kern_return_t kr = driver_->Create(driver_, "ASFWMidiNubProperties", &nubService);
    if (kr != kIOReturnSuccess || nubService == nullptr) {
        ASFW_LOG_ERROR(Midi, "MidiNubPublisher: Create failed endpoint=%llx kr=0x%x",
                       endpointId, kr);
        abandon();
        return false;
    }

    // Create has already started the nub, and its Start read nothing: the nub's
    // identity and properties are set now, explicitly, and only then published.
    auto* midiNub = OSDynamicCast(ASFWMidiNub, nubService);
    if (midiNub == nullptr) {
        ASFW_LOG_ERROR(Midi, "MidiNubPublisher: created service is not an ASFWMidiNub");
        nubService->release();
        abandon();
        return false;
    }
    midiNub->SetEndpointId(endpointId);

    OSDictionary* rawProperties = nullptr;
    kr = nubService->CopyProperties(&rawProperties);
    auto properties = OSSharedPtr(rawProperties, OSNoRetain);
    if (kr != kIOReturnSuccess || !properties) {
        ASFW_LOG_ERROR(Midi, "MidiNubPublisher: CopyProperties failed endpoint=%llx kr=0x%x",
                       endpointId, kr);
        nubService->Terminate(0);
        nubService->release();
        abandon();
        return false;
    }

    SetNumber(properties.get(), Keys::kGuid, caps.guid, 64);
    SetNumber(properties.get(), Keys::kEndpointId, endpointId, 64);
    SetString(properties.get(), Keys::kDeviceName, deviceName);
    SetString(properties.get(), Keys::kModel, model);
    SetString(properties.get(), Keys::kManufacturer, manufacturer);

    const auto& source = caps.deviceToHost;
    const auto& destination = caps.hostToDevice;
    SetNumber(properties.get(), Keys::kSourcePorts,
              source.Usable() ? source.portCount : 0, 32);
    SetNumber(properties.get(), Keys::kSourceSlotIndex, source.midiSlotIndex, 32);
    SetNumber(properties.get(), Keys::kSourceDbs, source.dbs, 32);
    SetNumber(properties.get(), Keys::kSourceStreamIndex, source.streamIndex, 32);
    SetNumber(properties.get(), Keys::kDestinationPorts,
              destination.Usable() ? destination.portCount : 0, 32);
    SetNumber(properties.get(), Keys::kDestinationSlotIndex, destination.midiSlotIndex, 32);
    SetNumber(properties.get(), Keys::kDestinationDbs, destination.dbs, 32);
    SetNumber(properties.get(), Keys::kDestinationStreamIndex, destination.streamIndex, 32);
    SetBool(properties.get(), Keys::kDbcAligned, source.dbcAligned && destination.dbcAligned);

    kr = nubService->SetProperties(properties.get());
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "MidiNubPublisher: SetProperties failed endpoint=%llx kr=0x%x",
                       endpointId, kr);
        nubService->Terminate(0);
        nubService->release();
        abandon();
        return false;
    }

    // Everything is in place: register for matching.
    kr = midiNub->Publish();
    if (kr != kIOReturnSuccess) {
        nubService->Terminate(0);
        nubService->release();
        abandon();
        return false;
    }

    IOLockLock(lock_);
    slot->nub = nubService;
    IOLockUnlock(lock_);
    // IOKit retains the published nub; drop the creation reference.
    nubService->release();

    ASFW_LOG(Midi,
             "MidiNubPublisher: published endpoint=%llx guid=0x%llx sources=%u destinations=%u",
             endpointId, caps.guid,
             source.Usable() ? source.portCount : 0,
             destination.Usable() ? destination.portCount : 0);
    return true;
}

void MidiNubPublisher::TerminateNub(const uint64_t endpointId) noexcept {
    if (lock_ == nullptr) return;
    IOService* nub = nullptr;
    IOLockLock(lock_);
    for (auto& entry : entries_) {
        if (!entry.used || entry.endpointId != endpointId) continue;
        nub = entry.nub;
        entry.used = false;
        entry.endpointId = 0;
        entry.nub = nullptr;
        break;
    }
    IOLockUnlock(lock_);
    if (nub != nullptr) {
        ASFW_LOG(Midi, "MidiNubPublisher: terminating endpoint=%llx", endpointId);
        nub->Terminate(0);
    }
}

void MidiNubPublisher::TerminateAll() noexcept {
    if (lock_ == nullptr) return;
    for (uint32_t i = 0; i < kMaxNubs; ++i) {
        uint64_t endpointId = 0;
        IOLockLock(lock_);
        if (entries_[i].used) endpointId = entries_[i].endpointId;
        IOLockUnlock(lock_);
        if (endpointId != 0) TerminateNub(endpointId);
    }
}

void MidiNubPublisher::MidiReceived(const uint64_t endpointId) noexcept {
    if (lock_ == nullptr) return;
    // The lock is held across the notify so a concurrent TerminateNub cannot
    // retire the nub between the lookup and the call. The notify is a one-way
    // action hand-off: it never waits on the MIDI service.
    IOLockLock(lock_);
    for (const auto& entry : entries_) {
        if (!entry.used || entry.endpointId != endpointId || entry.nub == nullptr) continue;
        if (auto* nub = OSDynamicCast(ASFWMidiNub, entry.nub)) {
            (void)nub->NotifyMidiReceived();
        }
        break;
    }
    IOLockUnlock(lock_);
}

} // namespace ASFW::Midi
