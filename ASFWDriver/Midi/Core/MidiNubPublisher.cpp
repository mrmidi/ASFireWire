//
// MidiNubPublisher.cpp
// ASFWDriver
//

#include "MidiNubPublisher.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSSharedPtr.h>

#include "../../Logging/Logging.hpp"
#include "MidiNubProperties.hpp"

namespace ASFW::Midi {

namespace {

namespace Keys = NubKeys;

void SetNumber(OSDictionary* properties, const char* key, uint64_t value,
               uint32_t bits) {
    auto number = OSSharedPtr(OSNumber::withNumber(value, bits), OSNoRetain);
    if (number) properties->setObject(key, number.get());
}

void SetString(OSDictionary* properties, const char* key, const char* value) {
    if (value == nullptr) return;
    auto text = OSSharedPtr(OSString::withCString(value), OSNoRetain);
    if (text) properties->setObject(key, text.get());
}

void SetBool(OSDictionary* properties, const char* key, bool value) {
    // DriverKit's OSBoolean has no withBoolean; the singletons are the only
    // way to make one, and they must not be released.
    OSBoolean* flag = value ? kOSBooleanTrue : kOSBooleanFalse;
    properties->setObject(key, flag);
}

} // namespace

// Bisection switch, 2026-09-14. Audio on a Saffire Pro 24 DSP went silent on
// the same day MIDI first published a live device, and the transmit path shows
// producer starvation rather than a transmit fault: identical producer code
// measured rebound=366/missedDeadline=0 before, and rebound~29000/
// missedDeadline=806 after, at equal fill. Publishing no MIDI nub stops the
// MIDI service starting, arming, and draining rings, which is the only way to
// tell whether it is taking time from the TX producer.
//
// Set back to false to restore MIDI. Nothing else is disabled: capability
// projection still runs, so the decision this gate makes is still logged.
static constexpr bool kSuppressMidiPublicationForAudioBisect = false;

bool MidiNubPublisher::EnsureNub(const MidiEndpointCapabilities& caps,
                                 const uint64_t endpointId,
                                 const char* deviceName,
                                 const char* model,
                                 const char* manufacturer) noexcept {
    if (driver_ == nullptr) return false;

    if (kSuppressMidiPublicationForAudioBisect) {
        ASFW_LOG(Midi,
                 "MidiNubPublisher: publication suppressed for audio bisect "
                 "(endpoint=%llu would publish sources=%u destinations=%u)",
                 endpointId,
                 caps.deviceToHost.Usable() ? caps.deviceToHost.portCount : 0,
                 caps.hostToDevice.Usable() ? caps.hostToDevice.portCount : 0);
        return true;
    }

    if (!caps.AnyUsable()) {
        // Normal for most FireWire audio interfaces. Say why once, at notice
        // level, so a device that should have MIDI and does not is visible
        // without turning on debug logging.
        ASFW_LOG(Midi,
                 "MidiNubPublisher: endpoint=%llu publishes no MIDI "
                 "(deviceToHost=%{public}s hostToDevice=%{public}s)",
                 endpointId,
                 MidiCapabilityStatusName(caps.deviceToHost.status),
                 MidiCapabilityStatusName(caps.hostToDevice.status));
        return true;
    }

    for (const auto& entry : entries_) {
        if (entry.used && entry.endpointId == endpointId) {
            return true;
        }
    }

    Entry* slot = nullptr;
    for (auto& entry : entries_) {
        if (!entry.used) { slot = &entry; break; }
    }
    if (slot == nullptr) {
        ASFW_LOG_ERROR(Midi, "MidiNubPublisher: no free slot for endpoint=%llu",
                       endpointId);
        return false;
    }

    IOService* nub = nullptr;
    kern_return_t kr = driver_->Create(driver_, "ASFWMidiNubProperties", &nub);
    if (kr != kIOReturnSuccess || nub == nullptr) {
        ASFW_LOG_ERROR(Midi,
                       "MidiNubPublisher: Create failed endpoint=%llu kr=0x%x",
                       endpointId, kr);
        return false;
    }

    // Create has already started the nub. Publish the endpoint properties now;
    // the nub resolves its endpoint identity when the first MIDI lease starts.
    OSDictionary* rawProperties = nullptr;
    kr = nub->CopyProperties(&rawProperties);
    auto properties = OSSharedPtr(rawProperties, OSNoRetain);
    if (kr != kIOReturnSuccess || !properties) {
        ASFW_LOG_ERROR(Midi,
                       "MidiNubPublisher: CopyProperties failed endpoint=%llu kr=0x%x",
                       endpointId, kr);
        nub->Terminate(0);
        return false;
    }

    SetNumber(properties.get(), Keys::kGuid, caps.guid, 64);
    SetNumber(properties.get(), Keys::kStreamEpoch, caps.streamEpoch, 64);
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
    SetNumber(properties.get(), Keys::kDestinationSlotIndex,
              destination.midiSlotIndex, 32);
    SetNumber(properties.get(), Keys::kDestinationDbs, destination.dbs, 32);
    SetNumber(properties.get(), Keys::kDestinationStreamIndex,
              destination.streamIndex, 32);
    SetBool(properties.get(), Keys::kDbcAligned,
            source.dbcAligned && destination.dbcAligned);

    kr = nub->SetProperties(properties.get());
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi,
                       "MidiNubPublisher: SetProperties failed endpoint=%llu kr=0x%x",
                       endpointId, kr);
        nub->Terminate(0);
        return false;
    }

    slot->used = true;
    slot->endpointId = endpointId;
    slot->nub = nub;

    ASFW_LOG(Midi,
             "MidiNubPublisher: published endpoint=%llu guid=0x%llx "
             "sources=%u destinations=%u",
             endpointId, caps.guid,
             source.Usable() ? source.portCount : 0,
             destination.Usable() ? destination.portCount : 0);
    return true;
}

IOService* MidiNubPublisher::GetNub(const uint64_t endpointId) const noexcept {
    for (const auto& entry : entries_) {
        if (entry.used && entry.endpointId == endpointId) return entry.nub;
    }
    return nullptr;
}

void MidiNubPublisher::TerminateNub(const uint64_t endpointId) noexcept {
    for (auto& entry : entries_) {
        if (!entry.used || entry.endpointId != endpointId) continue;
        if (entry.nub != nullptr) {
            ASFW_LOG(Midi, "MidiNubPublisher: terminating endpoint=%llu",
                     endpointId);
            entry.nub->Terminate(0);
        }
        entry.used = false;
        entry.endpointId = 0;
        entry.nub = nullptr;
        return;
    }
}

} // namespace ASFW::Midi
