//
// ASFWAudioNub.cpp
// ASFWDriver
//
// Implementation of audio nub published by ASFWDriver
// Manages shared memory for cross-process TX audio queue
//

#include "ASFWAudioNub.h"
#include "../../Logging/Logging.hpp"
#include "../../Shared/TxSharedQueue.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>

// TX queue capacity: 4096 frames = ~85ms @ 48kHz
// This provides good buffering without excessive latency
static constexpr uint32_t kTxQueueCapacityFrames = 4096;

// Helper to create and initialize the TX queue
static kern_return_t CreateTxQueue(ASFWAudioNub_IVars* iv)
{
    if (!iv) return kIOReturnBadArgument;

    // Already created?
    if (iv->txQueueMem && iv->txQueueMap) {
        return kIOReturnSuccess;
    }

    const uint64_t bytes = ASFW::Shared::TxSharedQueueSPSC::RequiredBytes(kTxQueueCapacityFrames);

    // Allocate IOBufferMemoryDescriptor
    IOBufferMemoryDescriptor* mem = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn,  // Bidirectional for producer/consumer
        bytes,
        64,  // 64-byte alignment for cache lines
        &mem);

    if (kr != kIOReturnSuccess || !mem) {
        ASFW_LOG(Audio, "ASFWAudioNub: Failed to create IOBufferMemoryDescriptor: 0x%x", kr);
        return kr ? kr : kIOReturnNoMemory;
    }

    kr = mem->SetLength(bytes);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: SetLength failed: 0x%x", kr);
        mem->release();
        return kr;
    }

    // Create local mapping for initialization and IT context access
    IOMemoryMap* map = nullptr;
    kr = mem->CreateMapping(
        kIOMemoryMapCacheModeDefault,
        0,    // offset
        0,    // address (0 = kernel chooses)
        0,    // length (0 = entire descriptor)
        0,    // options
        &map);

    if (kr != kIOReturnSuccess || !map) {
        ASFW_LOG(Audio, "ASFWAudioNub: CreateMapping failed: 0x%x", kr);
        mem->release();
        return kr ? kr : kIOReturnNoMemory;
    }

    // Initialize SPSC queue in shared memory
    void* base = reinterpret_cast<void*>(map->GetAddress());
    bool initOk = ASFW::Shared::TxSharedQueueSPSC::InitializeInPlace(base, bytes, kTxQueueCapacityFrames);
    if (!initOk) {
        ASFW_LOG(Audio, "ASFWAudioNub: TxSharedQueue initialization failed");
        map->release();
        mem->release();
        return kIOReturnError;
    }

    iv->txQueueMem = mem;    // retained
    iv->txQueueMap = map;    // retained
    iv->txQueueBytes = bytes;

    ASFW_LOG(Audio, "ASFWAudioNub: TX queue created: %llu bytes, %u frames capacity, base=%p",
             bytes, kTxQueueCapacityFrames, base);

    return kIOReturnSuccess;
}

bool ASFWAudioNub::init()
{
    bool result = super::init();
    if (!result) {
        ASFW_LOG(Audio, "ASFWAudioNub: super::init() failed");
        return false;
    }

    ivars = IONewZero(ASFWAudioNub_IVars, 1);
    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioNub: Failed to allocate ivars");
        return false;
    }

    ivars->parentDriver = nullptr;
    ivars->txQueueMem = nullptr;
    ivars->txQueueMap = nullptr;
    ivars->txQueueBytes = 0;

    ASFW_LOG(Audio, "ASFWAudioNub: init() succeeded");
    return true;
}

void ASFWAudioNub::free()
{
    ASFW_LOG(Audio, "ASFWAudioNub: free()");
    if (ivars) {
        // Release shared memory resources
        if (ivars->txQueueMap) {
            ivars->txQueueMap->release();
            ivars->txQueueMap = nullptr;
        }
        if (ivars->txQueueMem) {
            ivars->txQueueMem->release();
            ivars->txQueueMem = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWAudioNub_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(ASFWAudioNub, Start)
{
    kern_return_t error = Start(provider, SUPERDISPATCH);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: super::Start() failed: %d", error);
        return error;
    }

    // Store reference to parent driver (ASFWDriver)
    ivars->parentDriver = provider;

    // Create TX queue now so it's ready when AudioDriver starts
    error = CreateTxQueue(ivars);
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: Failed to create TX queue: 0x%x", error);
        return error;
    }

    // Properties were set by ASFWDriver before publishing this nub
    // ASFWAudioDriver will read them when it matches
    ASFW_LOG(Audio, "ASFWAudioNub: Started with TX queue ready");

    // Register the service so ASFWAudioDriver can match on us
    error = RegisterService();
    if (error != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: RegisterService() failed: %d", error);
        return error;
    }

    ASFW_LOG(Audio, "ASFWAudioNub[%p]: Started and registered", this);
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWAudioNub, Stop)
{
    ASFW_LOG(Audio, "ASFWAudioNub: Stop()");
    if (ivars) {
        ivars->parentDriver = nullptr;
        // Note: Don't release txQueueMem/Map here - they may still be in use
        // They will be released in free()
    }
    return Stop(provider, SUPERDISPATCH);
}

// RPC method callable from ASFWAudioDriver (different process)
kern_return_t IMPL(ASFWAudioNub, CopyTransmitQueueMemory)
{
    ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory called");

    if (!outMemory || !outBytes) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory: bad arguments");
        return kIOReturnBadArgument;
    }

    if (!ivars) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory: no ivars");
        return kIOReturnNotReady;
    }

    // Ensure TX queue exists
    kern_return_t kr = CreateTxQueue(ivars);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory: CreateTxQueue failed: 0x%x", kr);
        return kr;
    }

    // Return retained reference - caller must release
    ivars->txQueueMem->retain();
    *outMemory = ivars->txQueueMem;
    *outBytes = ivars->txQueueBytes;

    ASFW_LOG(Audio, "ASFWAudioNub: CopyTransmitQueueMemory: returning mem=%p bytes=%llu",
             ivars->txQueueMem, ivars->txQueueBytes);

    return kIOReturnSuccess;
}

// LOCALONLY: Get parent driver pointer (same process)
void* ASFWAudioNub::GetParentDriver() const
{
    return ivars ? ivars->parentDriver : nullptr;
}

// LOCALONLY: Get local mapping base address for IT context
void* ASFWAudioNub::GetTxQueueLocalMapping() const
{
    if (!ivars || !ivars->txQueueMap) {
        return nullptr;
    }
    return reinterpret_cast<void*>(ivars->txQueueMap->GetAddress());
}

// LOCALONLY: Get TX queue size
uint64_t ASFWAudioNub::GetTxQueueBytes() const
{
    return ivars ? ivars->txQueueBytes : 0;
}
