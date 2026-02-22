#include "AudioSharedMemoryBridge.hpp"

namespace ASFW::Isoch::Audio {
namespace {

kern_return_t MapSharedQueue(IOBufferMemoryDescriptor* queueMemory,
                             uint64_t queueBytes,
                             OSSharedPtr<IOBufferMemoryDescriptor>& outQueueMemory,
                             OSSharedPtr<IOMemoryMap>& outQueueMap,
                             uint64_t& outQueueBytes,
                             ASFW::Shared::TxSharedQueueSPSC& outQueue,
                             bool& outQueueValid) {
    outQueueValid = false;
    outQueueMap.reset();
    outQueueMemory.reset();
    outQueueBytes = 0;

    if (!queueMemory || queueBytes == 0) {
        return kIOReturnBadArgument;
    }

    outQueueMemory.reset(queueMemory, OSNoRetain);
    outQueueBytes = queueBytes;

    IOMemoryMap* queueMapRaw = nullptr;
    kern_return_t mappingStatus = queueMemory->CreateMapping(
        kIOMemoryMapCacheModeDefault,
        0,
        0,
        0,
        0,
        &queueMapRaw);
    if (mappingStatus != kIOReturnSuccess || !queueMapRaw) {
        outQueueMemory.reset();
        outQueueBytes = 0;
        return (mappingStatus == kIOReturnSuccess) ? kIOReturnNoMemory : mappingStatus;
    }

    outQueueMap.reset(queueMapRaw, OSNoRetain);

    void* baseAddress = reinterpret_cast<void*>(queueMapRaw->GetAddress());
    if (!outQueue.Attach(baseAddress, queueBytes)) {
        outQueueMap.reset();
        outQueueMemory.reset();
        outQueueBytes = 0;
        return kIOReturnInvalid;
    }

    outQueueValid = true;
    return kIOReturnSuccess;
}

} // namespace

kern_return_t MapRxQueueFromNub(ASFWAudioNub& nub,
                                OSSharedPtr<IOBufferMemoryDescriptor>& outQueueMemory,
                                OSSharedPtr<IOMemoryMap>& outQueueMap,
                                uint64_t& outQueueBytes,
                                ASFW::Shared::TxSharedQueueSPSC& outQueueReader,
                                bool& outQueueValid) {
    IOBufferMemoryDescriptor* queueMemory = nullptr;
    uint64_t queueBytes = 0;
    const kern_return_t copyStatus = nub.CopyRxQueueMemory(&queueMemory, &queueBytes);
    if (copyStatus != kIOReturnSuccess) {
        outQueueValid = false;
        return copyStatus;
    }

    return MapSharedQueue(queueMemory,
                          queueBytes,
                          outQueueMemory,
                          outQueueMap,
                          outQueueBytes,
                          outQueueReader,
                          outQueueValid);
}

kern_return_t MapTxQueueFromNub(ASFWAudioNub& nub,
                                OSSharedPtr<IOBufferMemoryDescriptor>& outQueueMemory,
                                OSSharedPtr<IOMemoryMap>& outQueueMap,
                                uint64_t& outQueueBytes,
                                ASFW::Shared::TxSharedQueueSPSC& outQueueWriter,
                                bool& outQueueValid) {
    IOBufferMemoryDescriptor* queueMemory = nullptr;
    uint64_t queueBytes = 0;
    const kern_return_t copyStatus = nub.CopyTransmitQueueMemory(&queueMemory, &queueBytes);
    if (copyStatus != kIOReturnSuccess) {
        outQueueValid = false;
        return copyStatus;
    }

    return MapSharedQueue(queueMemory,
                          queueBytes,
                          outQueueMemory,
                          outQueueMap,
                          outQueueBytes,
                          outQueueWriter,
                          outQueueValid);
}

kern_return_t MapZeroCopyOutputFromNub(bool enableZeroCopy,
                                       ASFWAudioNub& nub,
                                       uint32_t channelCount,
                                       OSSharedPtr<IOBufferMemoryDescriptor>& outStreamOutputBuffer,
                                       OSSharedPtr<IOBufferMemoryDescriptor>& outSharedOutputBuffer,
                                       OSSharedPtr<IOMemoryMap>& outSharedOutputMap,
                                       uint64_t& outSharedOutputBytes,
                                       uint32_t& outZeroCopyFrameCapacity,
                                       bool& outZeroCopyEnabled) {
    outZeroCopyEnabled = false;
    outSharedOutputMap.reset();
    outSharedOutputBuffer.reset();
    outSharedOutputBytes = 0;
    outZeroCopyFrameCapacity = 0;

    if (!enableZeroCopy) {
        return kIOReturnUnsupported;
    }

    IOBufferMemoryDescriptor* sharedOutputRaw = nullptr;
    uint64_t sharedOutputBytes = 0;
    kern_return_t copyStatus = nub.CopyOutputAudioMemory(&sharedOutputRaw, &sharedOutputBytes);
    if (copyStatus != kIOReturnSuccess || !sharedOutputRaw || sharedOutputBytes == 0) {
        return (copyStatus == kIOReturnSuccess) ? kIOReturnNoMemory : copyStatus;
    }

    outSharedOutputBuffer.reset(sharedOutputRaw, OSNoRetain);
    outSharedOutputBytes = sharedOutputBytes;

    IOMemoryMap* outputMapRaw = nullptr;
    kern_return_t mappingStatus = sharedOutputRaw->CreateMapping(
        kIOMemoryMapCacheModeDefault,
        0,
        0,
        0,
        0,
        &outputMapRaw);
    if (mappingStatus != kIOReturnSuccess || !outputMapRaw) {
        outSharedOutputBuffer.reset();
        outSharedOutputBytes = 0;
        return (mappingStatus == kIOReturnSuccess) ? kIOReturnNoMemory : mappingStatus;
    }

    outSharedOutputMap.reset(outputMapRaw, OSNoRetain);
    outZeroCopyEnabled = true;
    if (channelCount > 0) {
        outZeroCopyFrameCapacity = static_cast<uint32_t>(
            sharedOutputBytes / (sizeof(int32_t) * channelCount));
    }

    outStreamOutputBuffer.reset(sharedOutputRaw, OSRetain);
    return kIOReturnSuccess;
}

void ResetZeroCopyState(OSSharedPtr<IOBufferMemoryDescriptor>& inOutSharedOutputBuffer,
                        OSSharedPtr<IOMemoryMap>& inOutSharedOutputMap,
                        uint64_t& inOutSharedOutputBytes,
                        uint32_t& inOutZeroCopyFrameCapacity,
                        bool& inOutZeroCopyEnabled) {
    inOutZeroCopyEnabled = false;
    inOutSharedOutputMap.reset();
    inOutSharedOutputBuffer.reset();
    inOutSharedOutputBytes = 0;
    inOutZeroCopyFrameCapacity = 0;
}

} // namespace ASFW::Isoch::Audio
