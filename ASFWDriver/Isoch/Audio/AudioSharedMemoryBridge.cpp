#include "AudioSharedMemoryBridge.hpp"

#include "../../Common/DriverKitOwnership.hpp"

#include <utility>

namespace ASFW::Isoch::Audio {
namespace {

kern_return_t MapSharedQueue(OSSharedPtr<IOBufferMemoryDescriptor> queueMemory,
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

    outQueueMemory = std::move(queueMemory);
    outQueueBytes = queueBytes;

    const kern_return_t mappingStatus =
        Common::CreateSharedMapping(outQueueMemory, outQueueMap);
    if (mappingStatus != kIOReturnSuccess) {
        outQueueMemory.reset();
        outQueueBytes = 0;
        return mappingStatus;
    }

    uint8_t* baseAddress = reinterpret_cast<uint8_t*>(outQueueMap->GetAddress());
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
    IOBufferMemoryDescriptor* queueMemoryRaw = nullptr;
    uint64_t queueBytes = 0;
    const kern_return_t copyStatus = nub.CopyRxQueueMemory(&queueMemoryRaw, &queueBytes);
    if (copyStatus != kIOReturnSuccess) {
        if (queueMemoryRaw) {
            queueMemoryRaw->release();
        }
        outQueueValid = false;
        return copyStatus;
    }

    return MapSharedQueue(Common::AdoptRetained(queueMemoryRaw),
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
    IOBufferMemoryDescriptor* queueMemoryRaw = nullptr;
    uint64_t queueBytes = 0;
    const kern_return_t copyStatus = nub.CopyTransmitQueueMemory(&queueMemoryRaw, &queueBytes);
    if (copyStatus != kIOReturnSuccess) {
        if (queueMemoryRaw) {
            queueMemoryRaw->release();
        }
        outQueueValid = false;
        return copyStatus;
    }

    return MapSharedQueue(Common::AdoptRetained(queueMemoryRaw),
                          queueBytes,
                          outQueueMemory,
                          outQueueMap,
                          outQueueBytes,
                          outQueueWriter,
                          outQueueValid);
}

// Positional output parameters mirror the shared-memory mapping state being populated.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t MapZeroCopyOutputFromNub(bool enableZeroCopy,
                                       ASFWAudioNub& nub,
                                       uint32_t channelCount,
                                       OSSharedPtr<IOBufferMemoryDescriptor>& outStreamOutputBuffer,
                                       OSSharedPtr<IOBufferMemoryDescriptor>& outSharedOutputBuffer,
                                       OSSharedPtr<IOMemoryMap>& outSharedOutputMap,
                                       uint64_t& outSharedOutputBytes, // NOLINT(bugprone-easily-swappable-parameters)
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
        if (sharedOutputRaw) {
            sharedOutputRaw->release();
        }
        return (copyStatus == kIOReturnSuccess) ? kIOReturnNoMemory : copyStatus;
    }

    outSharedOutputBuffer = Common::AdoptRetained(sharedOutputRaw);
    outSharedOutputBytes = sharedOutputBytes;

    const kern_return_t mappingStatus =
        Common::CreateSharedMapping(outSharedOutputBuffer, outSharedOutputMap);
    if (mappingStatus != kIOReturnSuccess) {
        outSharedOutputBuffer.reset();
        outSharedOutputBytes = 0;
        return mappingStatus;
    }

    outZeroCopyEnabled = true;
    if (channelCount > 0) {
        outZeroCopyFrameCapacity = static_cast<uint32_t>(
            sharedOutputBytes / (sizeof(int32_t) * channelCount));
    }

    outStreamOutputBuffer = outSharedOutputBuffer;
    return kIOReturnSuccess;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ResetZeroCopyState(OSSharedPtr<IOBufferMemoryDescriptor>& inOutSharedOutputBuffer,
                        OSSharedPtr<IOMemoryMap>& inOutSharedOutputMap,
                        uint64_t& inOutSharedOutputBytes, // NOLINT(bugprone-easily-swappable-parameters)
                        uint32_t& inOutZeroCopyFrameCapacity,
                        bool& inOutZeroCopyEnabled) {
    inOutZeroCopyEnabled = false;
    inOutSharedOutputMap.reset();
    inOutSharedOutputBuffer.reset();
    inOutSharedOutputBytes = 0;
    inOutZeroCopyFrameCapacity = 0;
}

} // namespace ASFW::Isoch::Audio
