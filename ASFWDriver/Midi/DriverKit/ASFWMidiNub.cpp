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
    // Overridden by nothing today; the MIDI service installs its own handling
    // by polling the rings from its work queue. Kept as the explicit wake
    // point so WP-7 has somewhere to call that is not the receive queue.
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
