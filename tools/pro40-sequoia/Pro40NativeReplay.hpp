// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeReceivePacket.hpp"
#include "Pro40Replay.hpp"
#include "InputLevelEvidence.hpp"
#include "InputRecording.hpp"
#include "InputTimingEvidence.hpp"
#ifdef PRO40_STARTUP_TIMELINE
#include "InputStartupActivity.hpp"
#endif

// Composition of native packet views and Pro 40 content. No DMA ownership,
// I/O, callback registration or device register writes occur here.
namespace Pro40Packets {
enum class NativeReplayError { none, transport, framing, sequence };
class NativeReplay {
    ReplayQueue<> queue_;
    const unsigned channel_;
    const unsigned stream_;
    InputLevelEvidence levels_;
#ifdef PRO40_STARTUP_TIMELINE
    InputStartupActivity startupActivity_;
#endif
    InputRecording *recording_=nullptr;
    InputTimingEvidence<> *timing_=nullptr;
    const bool observeInput_;
    const uint8_t deviceNode_;
    NativeReplayError fault_ = NativeReplayError::none;
    NativePackets::Error transportError_ = NativePackets::Error::none;
    ReplayError sequenceError_ = ReplayError::none;
public:
    NativeReplay(unsigned stream, unsigned channel, uint8_t deviceNode,
                 uint8_t hostNode, uint32_t generation, InputRecording *recording=nullptr,InputTimingEvidence<> *timing=nullptr,bool observeInput=true,
                 const PlaybackClip *playback=nullptr,const PlaybackPcmSource *live=nullptr)
        : queue_(stream,deviceNode,hostNode,generation,playback,live), channel_(channel), stream_(stream), recording_(recording), timing_(timing), observeInput_(observeInput), deviceNode_(deviceNode) {
        if(queue_.fault()!=ReplayError::none) {
            fault_=NativeReplayError::sequence;sequenceError_=queue_.fault();
        }
        if(!observeInput_ && (recording_ || timing_)) {
            fault_=NativeReplayError::sequence;sequenceError_=ReplayError::state;
        }
    }
    const InputLevelEvidence &inputLevels() const { return levels_; }
#ifdef PRO40_STARTUP_TIMELINE
    const InputStartupActivity &startupActivity() const { return startupActivity_; }
#endif
    NativeReplayError fault() const { return fault_; }
    NativePackets::Error transportError() const { return transportError_; }
    ReplayError sequenceError() const { return sequenceError_; }
    size_t queued() const { return queue_.queued(); }
    size_t playbackFramesPrepared() const {return queue_.playbackFramesPrepared();}
    bool ready() const { return fault_ == NativeReplayError::none && queue_.ready(); }
    NativeReplayError ingest(std::span<const uint8_t> buffer, uint32_t status,
                            uint32_t timestamp, uint32_t generation) {
        if (fault_ != NativeReplayError::none) return fault_;
        const auto packet = NativePackets::receive(buffer,status,timestamp,channel_);
        transportError_ = packet.error;
        if (!packet) return fault_ = NativeReplayError::transport;
        if (packet.tag != 1 || packet.sync != 0) return fault_ = NativeReplayError::framing;
        sequenceError_ = queue_.ingest(packet.payload,packet.cycleStamp,generation);
        if (sequenceError_ != ReplayError::none) return fault_ = NativeReplayError::sequence;
        if(stream_==0 && observeInput_) {
            levels_.observe(packet.payload);
#ifdef PRO40_STARTUP_TIMELINE
            startupActivity_.observe(packet.payload);
#endif
            if(recording_)recording_->observe(packet.payload);
            if(timing_)timing_->observe(packet.payload,packet.cycleStamp,generation,deviceNode_);
        }
        return NativeReplayError::none;
    }
    ReplayError start(uint32_t cycleStamp, uint32_t generation) {
        if (fault_ != NativeReplayError::none) return ReplayError::state;
        const auto result = queue_.start(cycleStamp,generation);
        if (queue_.fault() != ReplayError::none) {
            fault_ = NativeReplayError::sequence; sequenceError_ = result;
        }
        return result;
    }
    ReplayResult produce(std::span<uint8_t> bytes, uint32_t cycleStamp, uint32_t generation) {
        if (fault_ != NativeReplayError::none) return {0,ReplayError::state};
        const auto result = queue_.produce(bytes,cycleStamp,generation);
        if (queue_.fault() != ReplayError::none) {
            fault_ = NativeReplayError::sequence; sequenceError_ = result.error;
        }
        return result;
    }
};
} // namespace Pro40Packets
