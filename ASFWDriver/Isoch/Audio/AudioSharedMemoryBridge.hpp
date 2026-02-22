#pragma once

#include "ASFWAudioNub.h"
#include "../../Shared/TxSharedQueue.hpp"

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::Isoch::Audio {

kern_return_t MapRxQueueFromNub(ASFWAudioNub& nub,
                                OSSharedPtr<IOBufferMemoryDescriptor>& outQueueMemory,
                                OSSharedPtr<IOMemoryMap>& outQueueMap,
                                uint64_t& outQueueBytes,
                                ASFW::Shared::TxSharedQueueSPSC& outQueueReader,
                                bool& outQueueValid);

kern_return_t MapTxQueueFromNub(ASFWAudioNub& nub,
                                OSSharedPtr<IOBufferMemoryDescriptor>& outQueueMemory,
                                OSSharedPtr<IOMemoryMap>& outQueueMap,
                                uint64_t& outQueueBytes,
                                ASFW::Shared::TxSharedQueueSPSC& outQueueWriter,
                                bool& outQueueValid);

kern_return_t MapZeroCopyOutputFromNub(bool enableZeroCopy,
                                       ASFWAudioNub& nub,
                                       uint32_t channelCount,
                                       OSSharedPtr<IOBufferMemoryDescriptor>& outStreamOutputBuffer,
                                       OSSharedPtr<IOBufferMemoryDescriptor>& outSharedOutputBuffer,
                                       OSSharedPtr<IOMemoryMap>& outSharedOutputMap,
                                       uint64_t& outSharedOutputBytes,
                                       uint32_t& outZeroCopyFrameCapacity,
                                       bool& outZeroCopyEnabled);

void ResetZeroCopyState(OSSharedPtr<IOBufferMemoryDescriptor>& inOutSharedOutputBuffer,
                        OSSharedPtr<IOMemoryMap>& inOutSharedOutputMap,
                        uint64_t& inOutSharedOutputBytes,
                        uint32_t& inOutZeroCopyFrameCapacity,
                        bool& inOutZeroCopyEnabled);

} // namespace ASFW::Isoch::Audio
