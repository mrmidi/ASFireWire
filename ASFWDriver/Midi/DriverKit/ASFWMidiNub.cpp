//
// ASFWMidiNub.cpp
// ASFWDriver
//

#define _LIBCPP_NO_ABI_TAG 1
#include <DriverKit/DriverKit.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWMidiNub.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriver.h>

#include "../../Logging/Logging.hpp"
#include "../../Service/DriverContext.hpp"
#include "../../Audio/Core/AudioCoordinator.hpp"
#include "../Core/MidiNubProperties.hpp"
#include "../Transport/MidiTransportBlock.hpp"

struct ASFWMidiNub_IVars {
    IOService* parentDriver{nullptr};
    uint64_t endpointId{0};
    OSSharedPtr<OSAction> midiReceiveAction;
    OSSharedPtr<IOBufferMemoryDescriptor> transportBuffer;
    OSSharedPtr<IOMemoryMap> transportMap;
    ASFW::Midi::MidiTransportBlock* block{nullptr};
    uint64_t streamEpoch{0};
};

bool ASFWMidiNub::init() {
    if (!super::init()) return false;
    ivars = IONewZero(ASFWMidiNub_IVars, 1);
    if (ivars == nullptr) return false;

    // Allocate the byte seam during init so that transport memory is valid
    // and can be armed before the service is registered.
    IOBufferMemoryDescriptor* rawBuffer = nullptr;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut, sizeof(ASFW::Midi::MidiTransportBlock),
        alignof(ASFW::Midi::MidiTransportBlock), &rawBuffer);
    if (ret != kIOReturnSuccess || rawBuffer == nullptr) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: transport alloc failed 0x%x", ret);
        return false;
    }
    ivars->transportBuffer = OSSharedPtr(rawBuffer, OSNoRetain);

    IOMemoryMap* rawMap = nullptr;
    ret = ivars->transportBuffer->CreateMapping(0, 0, 0, 0, 0, &rawMap);
    if (ret != kIOReturnSuccess || rawMap == nullptr) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: transport map failed 0x%x", ret);
        ivars->transportBuffer.reset();
        return false;
    }
    ivars->transportMap = OSSharedPtr(rawMap, OSNoRetain);

    // Placement-new is deliberate: the block holds atomics whose constructors
    // must run, and the descriptor's memory is raw.
    auto* block = reinterpret_cast<ASFW::Midi::MidiTransportBlock*>(
        static_cast<uintptr_t>(ivars->transportMap->GetAddress()));
#ifndef __clang_analyzer__
    new (block) ASFW::Midi::MidiTransportBlock();
#endif
    ivars->block = block;

    return true;
}

void ASFWMidiNub::free() {
    if (ivars != nullptr) {
        ivars->midiReceiveAction.reset();
        if (ivars->block != nullptr) {
            ivars->block->~MidiTransportBlock();
            ivars->block = nullptr;
        }
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

    if (ivars != nullptr) {
        ivars->parentDriver = provider;
        OSDictionary* rawProps = nullptr;
        if (CopyProperties(&rawProps) == kIOReturnSuccess && rawProps != nullptr) {
            auto props = OSSharedPtr(rawProps, OSNoRetain);
            auto* epNum = OSDynamicCast(OSNumber, props->getObject(ASFW::Midi::NubKeys::kEndpointId));
            if (epNum != nullptr) {
                ivars->endpointId = epNum->unsigned64BitValue();
            }
        }
    }

    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: RegisterService failed 0x%x", ret);
        return ret;
    }

    ASFW_LOG(Midi, "ASFWMidiNub: started, endpoint=%llu transport=%zu bytes",
             ivars ? ivars->endpointId : 0ULL,
             sizeof(ASFW::Midi::MidiTransportBlock));
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWMidiNub, Stop) {
    ASFW_LOG(Midi, "ASFWMidiNub: stopping");
    // Refuse further ring use before anything is torn down. The mapping itself
    // stays alive until free(), so a consumer already inside a callback sees a
    // quiesced block rather than unmapped memory.
    QuiesceTransport();
    if (ivars != nullptr) {
        ivars->parentDriver = nullptr;
    }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t IMPL(ASFWMidiNub, StartMidiStreaming) {
    if (ivars == nullptr) return kIOReturnNotReady;
    // IOService::Create has already run Start before MidiNubPublisher adds
    // endpoint properties. Resolve the published identity on the first lease,
    // then retain it for the matching stop even if properties are withdrawn.
    if (ivars->endpointId == 0) {
        OSDictionary* rawProps = nullptr;
        const kern_return_t propsKr = CopyProperties(&rawProps);
        auto props = OSSharedPtr(rawProps, OSNoRetain);
        if (propsKr != kIOReturnSuccess) return propsKr;
        auto* endpoint = props
            ? OSDynamicCast(OSNumber, props->getObject(ASFW::Midi::NubKeys::kEndpointId))
            : nullptr;
        if (endpoint != nullptr) ivars->endpointId = endpoint->unsigned64BitValue();
        if (ivars->endpointId == 0) {
            ASFW_LOG_ERROR(Midi, "ASFWMidiNub: cannot start MIDI without published endpoint identity");
            return kIOReturnNotReady;
        }
    }
    auto* parent = OSDynamicCast(ASFWDriver, ivars->parentDriver);
    if (!parent) return kIOReturnNotReady;
    auto* ctx = static_cast<ServiceContext*>(parent->GetServiceContext());
    if (!ctx || !ctx->audioCoordinator) return kIOReturnNotReady;

    return ctx->audioCoordinator->StartMidiStreaming(
        ASFW::Audio::Devices::AudioEndpointId{ivars->endpointId});
}

kern_return_t IMPL(ASFWMidiNub, StopMidiStreaming) {
    if (ivars == nullptr || ivars->endpointId == 0) return kIOReturnNotReady;
    auto* parent = OSDynamicCast(ASFWDriver, ivars->parentDriver);
    if (!parent) return kIOReturnNotReady;
    auto* ctx = static_cast<ServiceContext*>(parent->GetServiceContext());
    if (!ctx || !ctx->audioCoordinator) return kIOReturnNotReady;

    return ctx->audioCoordinator->StopMidiStreaming(
        ASFW::Audio::Devices::AudioEndpointId{ivars->endpointId});
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
