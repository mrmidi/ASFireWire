//
// ASFWMIDIDriver.cpp
// ASFWDriver
//

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <MIDIDriverKit/MIDIDriverKit.h>

#include "ASFWMIDIDriver.h"
#include "ASFWMidiDevice.h"
#include "ASFWMidiNub.h"

#include "../../Common/DriverKitOwnership.hpp"
#include "../../Logging/Logging.hpp"
#include "../Core/MidiNubProperties.hpp"
#include "../Transport/MidiTransportBlock.hpp"

struct ASFWMIDIDriver_IVars {
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<ASFWMidiDevice> device;
    // Retained for this service's whole lifetime. The descriptor belongs to the
    // endpoint; dropping either of these while a callback is in flight is
    // exactly the FW-60 failure, so they outlive the device deliberately.
    OSSharedPtr<IOMemoryDescriptor> transportBuffer;
    OSSharedPtr<IOMemoryMap> transportMap;
    OSSharedPtr<OSAction> receiveAction;
    ASFWMidiNub* nub{nullptr};
    uint64_t guid{0};
};

namespace {

namespace Keys = ASFW::Midi::NubKeys;

/// The nub's whole property dictionary. DriverKit's IOService exposes
/// CopyProperties, not a per-key CopyProperty, so read it once and look keys up
/// locally rather than paying a copy per field.
OSSharedPtr<OSDictionary> ReadNubProperties(IOService* provider) {
    if (provider == nullptr) return {};
    OSDictionary* raw = nullptr;
    if (provider->CopyProperties(&raw) != kIOReturnSuccess || raw == nullptr) {
        return {};
    }
    return OSSharedPtr(raw, OSNoRetain);
}

uint64_t NumberProperty(OSDictionary* properties, const char* key, uint64_t fallback) {
    if (properties == nullptr) return fallback;
    auto* number = OSDynamicCast(OSNumber, properties->getObject(key));
    return (number != nullptr) ? number->unsigned64BitValue() : fallback;
}

OSSharedPtr<OSString> StringProperty(OSDictionary* properties, const char* key,
                                     const char* fallback) {
    if (properties != nullptr) {
        if (auto* text = OSDynamicCast(OSString, properties->getObject(key))) {
            return OSSharedPtr(OSString::withString(text), OSNoRetain);
        }
    }
    return OSSharedPtr(OSString::withCString(fallback), OSNoRetain);
}

} // namespace

bool ASFWMIDIDriver::init() {
    if (!super::init()) return false;
    ivars = IONewZero(ASFWMIDIDriver_IVars, 1);
    return ivars != nullptr;
}

void ASFWMIDIDriver::free() {
    ASFW_LOG(Midi, "[Lifecycle] ASFWMIDIDriver free obj=%p", this);
    if (ivars != nullptr) {
        ivars->workQueue.reset();
        ivars->device.reset();
    }
    IOSafeDeleteNULL(ivars, ASFWMIDIDriver_IVars, 1);
    super::free();
}

kern_return_t IMPL(ASFWMIDIDriver, Start) {
    kern_return_t error = Start(provider, SUPERDISPATCH);
    if (error != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: super::Start failed 0x%x", error);
        return error;
    }

    ivars->workQueue = GetWorkQueue();
    if (!ivars->workQueue) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: no work queue");
        return kIOReturnInvalid;
    }

    // Everything about the endpoint arrives as properties the publisher set
    // before the nub registered, so there is no call back across the seam here.
    auto properties = ReadNubProperties(provider);
    const uint32_t sourcePorts = static_cast<uint32_t>(
        NumberProperty(properties.get(), Keys::kSourcePorts, 0));
    const uint32_t destinationPorts = static_cast<uint32_t>(
        NumberProperty(properties.get(), Keys::kDestinationPorts, 0));
    ivars->guid = NumberProperty(properties.get(), Keys::kGuid, 0);

    if (sourcePorts == 0 && destinationPorts == 0) {
        // The projection rejected both directions, or the device has no MIDI.
        // Starting with no endpoints would publish an empty CoreMIDI device
        // that can never do anything, so decline the match instead.
        ASFW_LOG(Midi,
                 "ASFWMIDIDriver: endpoint guid=0x%llx reports no usable MIDI; not publishing",
                 ivars->guid);
        return kIOReturnUnsupported;
    }

    auto deviceName = StringProperty(properties.get(), Keys::kDeviceName, "ASFW MIDI");
    auto modelUID = StringProperty(properties.get(), Keys::kModel, "ASFW");
    auto manufacturerUID =
        StringProperty(properties.get(), Keys::kManufacturer, "ASFireWire");
    if (!deviceName || !modelUID || !manufacturerUID) {
        return kIOReturnNoMemory;
    }

    // The personality matches only ASFWMidiNub, so the provider is one; the
    // audio side casts the same way (ASFWAudioDriverGraph.cpp).
    auto* nub = reinterpret_cast<ASFWMidiNub*>(provider);
    ivars->nub = nub;

    // Map the byte seam BEFORE any device exists. If the endpoint has no seam
    // (or it cannot be mapped) there is nothing this service can carry: decline
    // the match rather than publish a device whose endpoints can never move a
    // byte.
    IOMemoryDescriptor* rawTransport = nullptr;
    error = nub->CopyMidiTransportMemory(&rawTransport);
    if (error != kIOReturnSuccess || rawTransport == nullptr) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: no transport memory from nub kr=0x%x", error);
        return error != kIOReturnSuccess ? error : kIOReturnNotReady;
    }
    ivars->transportBuffer = ASFW::Common::AdoptRetained(rawTransport);
    if (ASFW::Common::CreateSharedMapping(ivars->transportBuffer, ivars->transportMap) !=
            kIOReturnSuccess ||
        !ivars->transportMap ||
        ivars->transportMap->GetLength() < sizeof(ASFW::Midi::MidiTransportBlock)) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: transport mapping failed");
        ivars->transportMap.reset();
        ivars->transportBuffer.reset();
        return kIOReturnNoMemory;
    }
    auto* block = reinterpret_cast<ASFW::Midi::MidiTransportBlock*>(
        static_cast<uintptr_t>(ivars->transportMap->GetAddress()));
    if (block->layoutVersion.load(std::memory_order_relaxed) !=
        ASFW::Midi::MidiTransportBlock::kLayoutVersion) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: transport layout %u, expected %u",
                       block->layoutVersion.load(std::memory_order_relaxed),
                       ASFW::Midi::MidiTransportBlock::kLayoutVersion);
        ivars->transportMap.reset();
        ivars->transportBuffer.reset();
        return kIOReturnUnsupported;
    }

    // OSTypeAlloc + init, not IOUserMIDIDevice::Create: the header reserves
    // Create for the un-subclassed case.
    ivars->device = OSSharedPtr(OSTypeAlloc(ASFWMidiDevice), OSNoRetain);
    if (!ivars->device) return kIOReturnNoMemory;

    if (!ivars->device->init(this, deviceName.get(), modelUID.get(),
                             manufacturerUID.get(), sourcePorts, destinationPorts)) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: device init failed");
        ivars->device.reset();
        return kIOReturnNoMemory;
    }

    error = AddObject(ivars->device.get());
    if (error != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: AddObject failed 0x%x", error);
        ivars->device.reset();
        return error;
    }
    // Same order the hardware-proven midi branch used: the device is added, then
    // its destinations get their I/O blocks. An I/O block that fires before this
    // line finds the device unbound and returns NotReady.
    ivars->device->BindTransport(block);

    // Register the receive wake. Without it the rings still fill, but nothing
    // drains them until a later wake from another source -- so a failure here is
    // reported, not ignored.
    OSAction* rawAction = nullptr;
    error = CreateActionMidiReceiveReady(0, &rawAction);
    if (error == kIOReturnSuccess && rawAction != nullptr) {
        ivars->receiveAction = OSSharedPtr(rawAction, OSNoRetain);
        error = nub->RegisterMidiReceiveAction(ivars->receiveAction.get());
        if (error != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: receive action registration failed 0x%x", error);
        }
    } else {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: receive action create failed 0x%x", error);
    }

    error = RegisterService();
    if (error != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: RegisterService failed 0x%x", error);
        (void)nub->RegisterMidiReceiveAction(nullptr);
        (void)ivars->device->UnbindTransport();
        ivars->device.reset();
        return error;
    }

    ASFW_LOG(Midi, "ASFWMIDIDriver: started guid=0x%llx sources=%u destinations=%u",
             ivars->guid, sourcePorts, destinationPorts);
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWMIDIDriver, Stop) {
    ASFW_LOG(Midi, "[Lifecycle] ASFWMIDIDriver Stop enter obj=%p provider=%p guid=0x%llx",
             this, provider, ivars ? ivars->guid : 0);
    // Order matters. Withdraw the wake first so the core driver stops handing us
    // work; then stop using the rings; then stop the service; and only then drop
    // the mapping. Releasing the mapping first would leave a callback that is
    // already running dereferencing unmapped memory.
    if (ivars != nullptr && ivars->nub != nullptr) {
        (void)ivars->nub->RegisterMidiReceiveAction(nullptr);
    }
    bool quiesced = true;
    if (ivars != nullptr && ivars->device) {
        ASFW_LOG(Midi, "[Lifecycle] ASFWMIDIDriver StopIO obj=%p", this);
        (void)ivars->device->StopIO();
        ASFW_LOG(Midi, "[Lifecycle] ASFWMIDIDriver UnbindTransport obj=%p", this);
        quiesced = ivars->device->UnbindTransport();
        ASFW_LOG(Midi, "[Lifecycle] ASFWMIDIDriver transport unbound obj=%p quiesced=%d", this, int(quiesced));
    }
    const kern_return_t ret = Stop(provider, SUPERDISPATCH);
    ASFW_LOG(Midi, "[Lifecycle] ASFWMIDIDriver Stop done obj=%p kr=0x%x", this, ret);
    if (ivars != nullptr) {
        ivars->nub = nullptr;
        ivars->device.reset();
        ivars->receiveAction.reset();
        ivars->workQueue.reset();
        if (quiesced) {
            ivars->transportMap.reset();
            ivars->transportBuffer.reset();
        } else {
            ASFW_LOG_ERROR(Midi,
                           "ASFWMIDIDriver: unbind timed out with active writers; "
                           "retaining the transport mapping to avoid a use-after-free");
        }
    }
    return ret;
}

kern_return_t IMPL(ASFWMIDIDriver, NewUserClient) {
    if (type == kIOUserMIDIDriverUserClientType) {
        // CoreMIDI's own connection. Forward to super, which creates the
        // IOUserMIDIDriverUserClient named in the personality.
        const kern_return_t error = super::NewUserClient(type, userClient, SUPERDISPATCH);
        if (error != kIOReturnSuccess || *userClient == nullptr) {
            ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: MIDI user client failed 0x%x", error);
            return error != kIOReturnSuccess ? error : kIOReturnNoMemory;
        }
        return kIOReturnSuccess;
    }

    // ASFW publishes no custom MIDI user client: the control app reaches the
    // driver through ASFWDriverUserClient on the core service. Refusing here
    // keeps that the only path rather than quietly opening a second one.
    ASFW_LOG(Midi, "ASFWMIDIDriver: refusing user client type %u", type);
    return kIOReturnUnsupported;
}

kern_return_t ASFWMIDIDriver::StartIO(OSArray* deviceList) {
    kern_return_t error = super::StartIO(deviceList);
    if (error != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMIDIDriver: super::StartIO failed 0x%x", error);
        return error;
    }
    if (ivars == nullptr || !ivars->device) return kIOReturnSuccess;
    return ivars->device->StartIO();
}

void IMPL(ASFWMIDIDriver, MidiReceiveReady) {
    (void)action;
    if (ivars == nullptr || !ivars->device) return;
    // Already on this service's queue: the action is what moved us off the core
    // driver's receive queue.
    ivars->device->DrainReceiveRings();
}

kern_return_t ASFWMIDIDriver::StopIO() {
    // Device first, then super: the mirror of StartIO.
    if (ivars != nullptr && ivars->device) {
        (void)ivars->device->StopIO();
    }
    return super::StopIO();
}
