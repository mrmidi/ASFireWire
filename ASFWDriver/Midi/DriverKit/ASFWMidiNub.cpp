//
// ASFWMidiNub.cpp
// ASFWDriver
//

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWMidiNub.h>

#include "../../Logging/Logging.hpp"
#include "../Transport/MidiTransportBlock.hpp"

struct ASFWMidiNub_IVars {
    OSSharedPtr<OSAction> midiReceiveAction;
    OSSharedPtr<IOBufferMemoryDescriptor> transportBuffer;
    OSSharedPtr<IOMemoryMap> transportMap;
    ASFW::Midi::MidiTransportBlock* block{nullptr};
    uint64_t streamEpoch{0};
};

bool ASFWMidiNub::init() {
    if (!super::init()) return false;
    ivars = IONewZero(ASFWMidiNub_IVars, 1);
    return ivars != nullptr;
}

void ASFWMidiNub::free() {
    if (ivars != nullptr) {
        ivars->midiReceiveAction.reset();
        ivars->block = nullptr;
        ivars->transportMap.reset();
        ivars->transportBuffer.reset();
    }
    IOSafeDeleteNULL(ivars, ASFWMidiNub_IVars, 1);
    super::free();
}

kern_return_t IMPL(ASFWMidiNub, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: super::Start failed 0x%x", ret);
        return ret;
    }

    // Allocate the byte seam before publishing, so a MIDI service that matches
    // immediately never sees a nub whose rings do not exist yet.
    IOBufferMemoryDescriptor* rawBuffer = nullptr;
    ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut, sizeof(ASFW::Midi::MidiTransportBlock),
        alignof(ASFW::Midi::MidiTransportBlock), &rawBuffer);
    if (ret != kIOReturnSuccess || rawBuffer == nullptr) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: transport alloc failed 0x%x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    ivars->transportBuffer = OSSharedPtr(rawBuffer, OSNoRetain);

    IOMemoryMap* rawMap = nullptr;
    ret = ivars->transportBuffer->CreateMapping(0, 0, 0, 0, 0, &rawMap);
    if (ret != kIOReturnSuccess || rawMap == nullptr) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: transport map failed 0x%x", ret);
        ivars->transportBuffer.reset();
        return ret != kIOReturnSuccess ? ret : kIOReturnNoMemory;
    }
    ivars->transportMap = OSSharedPtr(rawMap, OSNoRetain);

    // Placement-new is deliberate: the block holds atomics whose constructors
    // must run, and the descriptor's memory is raw.
    auto* address = reinterpret_cast<void*>(ivars->transportMap->GetAddress());
    ivars->block = new (address) ASFW::Midi::MidiTransportBlock();

    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: RegisterService failed 0x%x", ret);
        return ret;
    }

    ASFW_LOG(Midi, "ASFWMidiNub: started, transport=%zu bytes",
             sizeof(ASFW::Midi::MidiTransportBlock));
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWMidiNub, Stop) {
    ASFW_LOG(Midi, "ASFWMidiNub: stopping");
    // Refuse further ring use before anything is torn down. The mapping itself
    // stays alive until free(), so a consumer already inside a callback sees a
    // quiesced block rather than unmapped memory.
    QuiesceTransport();
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(ASFWMidiNub, CopyMidiTransportMemory) {
    if (ivars == nullptr || !ivars->transportBuffer) return kIOReturnNotReady;
    if (outMemory == nullptr) return kIOReturnBadArgument;

    ivars->transportBuffer->retain();
    *outMemory = ivars->transportBuffer.get();
    if (outStreamEpoch != nullptr) *outStreamEpoch = ivars->streamEpoch;
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWMidiNub, NotifyMidiReceived) {
    if (ivars == nullptr || !ivars->midiReceiveAction) {
        // No MIDI service attached. Bytes stay queued; they are not lost, and
        // a service that attaches later drains whatever accumulated.
        return kIOReturnNotReady;
    }
    // Hands off to the MIDI service's queue and returns. This runs on the
    // receive queue, which must not be held while a CoreMIDI client is served.
    MidiReceiveReady(ivars->midiReceiveAction.get());
    return kIOReturnSuccess;
}

void IMPL(ASFWMidiNub, MidiReceiveReady) {
    // Originated here, handled by ASFWMIDIDriver's override.
    (void)action;
}

kern_return_t IMPL(ASFWMidiNub, RegisterMidiReceiveAction) {
    if (ivars == nullptr) return kIOReturnNotReady;
    ivars->midiReceiveAction = OSSharedPtr(action, OSRetain);
    return kIOReturnSuccess;
}

void* ASFWMidiNub::GetTransportBlock() {
    return ivars ? static_cast<void*>(ivars->block) : nullptr;
}

void ASFWMidiNub::ArmTransport(uint64_t streamEpoch) {
    if (ivars == nullptr || ivars->block == nullptr) return;
    ivars->streamEpoch = streamEpoch;
    ivars->block->Arm(streamEpoch);
    ASFW_LOG(Midi, "ASFWMidiNub: transport armed epoch=%llu", streamEpoch);
}

void ASFWMidiNub::QuiesceTransport() {
    if (ivars == nullptr || ivars->block == nullptr) return;
    ivars->block->Quiesce();
    ASFW_LOG(Midi, "ASFWMidiNub: transport quiesced");
}
