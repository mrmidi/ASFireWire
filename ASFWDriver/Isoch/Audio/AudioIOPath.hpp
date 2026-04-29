#pragma once

#include "../../Shared/TxSharedQueue.hpp"
#include "../Encoding/PacketAssembler.hpp"

#include <AudioDriverKit/AudioDriverKit.h>
#include <DriverKit/DriverKit.h>

#include <cstdint>

namespace ASFW::Isoch::Audio {

struct ZeroCopyTimelineState {
    bool valid{false};
    uint64_t lastSampleTime{0};
    uint64_t publishedSampleTime{0};
    uint64_t discontinuities{0};
    uint32_t phaseFrames{0};
};

struct AudioIOPathState {
    IOBufferMemoryDescriptor* inputBuffer{nullptr};
    IOBufferMemoryDescriptor* outputBuffer{nullptr};

    uint32_t inputChannelCount{0};
    uint32_t outputChannelCount{0};
    uint32_t ioBufferPeriodFrames{0};

    bool* rxStartupDrained{nullptr};
    bool* rxTransportRebased{nullptr};
    bool rxQueueValid{false};
    ASFW::Shared::TxSharedQueueSPSC* rxQueueReader{nullptr};

    bool txQueueValid{false};
    ASFW::Shared::TxSharedQueueSPSC* txQueueWriter{nullptr};

    bool zeroCopyEnabled{false};
    uint32_t zeroCopyFrameCapacity{0};
    ZeroCopyTimelineState* zeroCopyTimeline{nullptr};

    ASFW::Encoding::PacketAssembler* packetAssembler{nullptr};
    uint64_t* encodingOverruns{nullptr};

    // Optional per-callback TX diagnostics, owned by the caller. These are
    // filled only for IOUserAudioIOOperationWriteEnd.
    uint32_t* writeEndFramesRequested{nullptr};
    uint32_t* writeEndFramesWritten{nullptr};
};

kern_return_t HandleIOOperation(AudioIOPathState& state,
                                IOUserAudioIOOperation operation,
                                uint32_t ioBufferFrameSize,
                                uint64_t sampleTime);

} // namespace ASFW::Isoch::Audio
