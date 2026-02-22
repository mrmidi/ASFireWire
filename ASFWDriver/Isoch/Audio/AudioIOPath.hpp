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

    uint32_t channelCount{0};
    uint32_t ioBufferPeriodFrames{0};

    bool* rxStartupDrained{nullptr};
    bool rxQueueValid{false};
    ASFW::Shared::TxSharedQueueSPSC* rxQueueReader{nullptr};

    bool txQueueValid{false};
    ASFW::Shared::TxSharedQueueSPSC* txQueueWriter{nullptr};

    bool zeroCopyEnabled{false};
    uint32_t zeroCopyFrameCapacity{0};
    ZeroCopyTimelineState* zeroCopyTimeline{nullptr};

    ASFW::Encoding::PacketAssembler* packetAssembler{nullptr};
    uint64_t* encodingOverruns{nullptr};
};

kern_return_t HandleIOOperation(AudioIOPathState& state,
                                IOUserAudioIOOperation operation,
                                uint32_t ioBufferFrameSize,
                                uint64_t sampleTime);

} // namespace ASFW::Isoch::Audio
