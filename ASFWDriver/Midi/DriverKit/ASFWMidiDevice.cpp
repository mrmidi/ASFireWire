//
// ASFWMidiDevice.cpp
// ASFWDriver
//

#include <cstdio>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <MIDIDriverKit/MIDIDriverKit.h>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWMidiDevice.h>

#include "../../Logging/Logging.hpp"

struct ASFWMidiDevice_IVars {
    OSSharedPtr<IOUserMIDIDriver> driver;
    OSSharedPtr<IODispatchQueue> workQueue;
    uint32_t sourcePorts{0};
    uint32_t destinationPorts{0};
};

namespace {

/// "ASFW MIDI 1", "ASFW MIDI 2" ... one per physical jack pair.
OSSharedPtr<OSString> MakeEntityName(uint32_t oneBasedIndex) {
    char name[32];
    std::snprintf(name, sizeof(name), "MIDI %u", oneBasedIndex);
    return OSSharedPtr(OSString::withCString(name), OSNoRetain);
}

} // namespace

bool ASFWMidiDevice::init(IOUserMIDIDriver* driver,
                          OSString* deviceUID,
                          OSString* modelUID,
                          OSString* manufacturerUID,
                          uint32_t sourcePorts,
                          uint32_t destinationPorts) {
    if (!super::init(driver, deviceUID, modelUID, manufacturerUID)) {
        return false;
    }
    ivars = IONewZero(ASFWMidiDevice_IVars, 1);
    if (ivars == nullptr) return false;

    ivars->driver = OSSharedPtr(driver, OSRetain);
    ivars->workQueue = GetWorkQueue();
    ivars->sourcePorts = sourcePorts;
    ivars->destinationPorts = destinationPorts;

    // One entity per physical jack pair. A device with two inputs and one
    // output publishes two entities, the second with a source and no
    // destination -- not a phantom destination whose bytes would go nowhere.
    const uint32_t entities =
        sourcePorts > destinationPorts ? sourcePorts : destinationPorts;
    for (uint32_t i = 0; i < entities; ++i) {
        const uint32_t sources = (i < sourcePorts) ? 1u : 0u;
        const uint32_t destinations = (i < destinationPorts) ? 1u : 0u;
        auto name = MakeEntityName(i + 1);
        if (!name) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: entity %u name alloc failed", i);
            return false;
        }
        // MIDIProtocol_1_0, not the sample's 2_0: the wire carries a MIDI 1.0
        // byte stream, so 1.0 is the honest declaration of what reaches the
        // device. Translation to UMP happens at the seam, and claiming 2.0
        // would promise resolution the hardware cannot carry.
        auto entity = IOUserMIDIEntity::Create(
            ivars->driver.get(), this, name.get(),
            IOUserMIDIProtocolID::MIDIProtocol_1_0, sources, destinations);
        if (!entity) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: entity %u create failed", i);
            return false;
        }
        const kern_return_t ret = AddEntity(entity.get());
        if (ret != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: AddEntity %u failed 0x%x", i, ret);
            return false;
        }
    }

    // The device exists but no FireWire stream is running yet. StartIO clears
    // this, so CoreMIDI shows the endpoints greyed until bytes can actually
    // flow.
    auto offline = OSSharedPtr(OSNumber::withNumber(uint64_t{1}, 32), OSNoRetain);
    if (offline) SetProperty(IOUserMIDIProperty::Offline, offline.get());

    ASFW_LOG(Midi,
             "ASFWMidiDevice: init entities=%u sources=%u destinations=%u",
             entities, sourcePorts, destinationPorts);
    return true;
}

void ASFWMidiDevice::free() {
    if (ivars != nullptr) {
        ivars->driver.reset();
        ivars->workQueue.reset();
    }
    IOSafeDeleteNULL(ivars, ASFWMidiDevice_IVars, 1);
    super::free();
}

uint32_t ASFWMidiDevice::SourcePortCount() const {
    return ivars ? ivars->sourcePorts : 0;
}

uint32_t ASFWMidiDevice::DestinationPortCount() const {
    return ivars ? ivars->destinationPorts : 0;
}

void ASFWMidiDevice::InstallLoopbackForBringUp() {
    auto entities = GetEntities();
    if (!entities) return;
    entities->iterateObjects(^bool(OSObject* object) {
        auto* entity = OSDynamicCast(IOUserMIDIEntity, object);
        if (entity == nullptr) return false;
        auto source = entity->GetSource(0);
        auto destination = entity->GetDestination(0);
        // Only an entity with both ends can loop back. One-directional
        // entities are left without a block rather than given one that
        // dereferences a null source on the real-time thread.
        if (!source || !destination) return false;
        auto block = ^kern_return_t(IOUserMIDIUMPWord const* umpWords,
                                    size_t numWords) {
            return source->Send(umpWords, numWords);
        };
        const kern_return_t ret = destination->SetIOBlock(block);
        if (ret != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: SetIOBlock failed 0x%x", ret);
        }
        return false;
    });
}

kern_return_t ASFWMidiDevice::StartIO() {
    __block kern_return_t error = kIOReturnSuccess;
    if (!ivars || !ivars->workQueue) return kIOReturnNotReady;

    ivars->workQueue->DispatchSync(^{
        error = super::StartIO();
        if (error != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: StartIO failed 0x%x", error);
            super::StopIO();
        }
    });

    if (error == kIOReturnSuccess) {
        // Set outside the DispatchSync above: the sample does the same, and a
        // property write is not part of the I/O state transition the queue is
        // serialising.
        auto online = OSSharedPtr(OSNumber::withNumber(uint64_t{0}, 32), OSNoRetain);
        if (online) SetProperty(IOUserMIDIProperty::Offline, online.get());
        ASFW_LOG(Midi, "ASFWMidiDevice: IO started");
    }
    return error;
}

kern_return_t ASFWMidiDevice::StopIO() {
    __block kern_return_t error = kIOReturnSuccess;
    if (!ivars || !ivars->workQueue) return kIOReturnNotReady;

    auto offline = OSSharedPtr(OSNumber::withNumber(uint64_t{1}, 32), OSNoRetain);
    if (offline) SetProperty(IOUserMIDIProperty::Offline, offline.get());

    ivars->workQueue->DispatchSync(^{
        error = super::StopIO();
    });
    if (error != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiDevice: StopIO failed 0x%x", error);
    } else {
        ASFW_LOG(Midi, "ASFWMidiDevice: IO stopped");
    }
    return error;
}
