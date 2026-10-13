// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Pro40CaptureStartup.hpp"
#include "PlaybackPcmSource.hpp"

// Content-side sequence history, independent of native DMA. Behavior checked
// against Linux amdtp-stream.c:469-625 (received sequence cache) and 2015-2046
// (pair streams by index). Fresh implementation; no reference code copied.
// A single event-loop owner must serialize calls. No allocation or logging.
namespace Pro40Packets {
enum class ReplayError {
    none, generation, packet, receiveCycle, continuity, missingSyt,
    overflow, underrun, outputCycle, state, capacity, source
};
struct ReplayResult {
    size_t bytes = 0;
    ReplayError error = ReplayError::none;
    explicit operator bool() const { return error == ReplayError::none; }
};

// Arrival/output stamps must be normalized by the transport to seconds*8000 +
// cycle, modulo eight seconds. This is NOT a raw OHCI word or NuDCL timestamp.
// The transport must also enforce a wall-time watchdog: a whole eight-second
// loss cannot be distinguished by modular stamps alone.
inline constexpr uint32_t cycleStampDomain = 8 * 8000;
constexpr uint32_t nextCycleStamp(uint32_t stamp) {
    return (stamp + 1) % cycleStampDomain;
}

template<size_t Capacity = 256, size_t Preroll = 32>
class ReplayQueue {
    static_assert(Preroll > 0 && Preroll <= Capacity && Capacity < 8000);
    struct Entry { uint8_t blocks = 0; uint32_t phase = 0; };
    std::array<Entry, Capacity> entries_{};
    const unsigned stream_;
    const uint8_t deviceNode_;
    const uint32_t generation_;
    Am824Stream encoder_;
    const PlaybackClip *clip_=nullptr;
    const PlaybackPcmSource *live_=nullptr;
    size_t playbackFrame_=0;
    uint64_t preparedPackets_=0;
    Continuity continuity_;
    CaptureStartup startup_;
    size_t head_ = 0, count_ = 0;
    uint32_t nextReceive_ = 0, nextOutput_ = 0;
    bool received_ = false, running_ = false;
    ReplayError fault_ = ReplayError::none;
    ReplayError fail(ReplayError error) { fault_ = error; return error; }
public:
    // Construct a fresh queue after a stop/reset; never silently discard a fault.
    ReplayQueue(unsigned stream, uint8_t deviceNode, uint8_t hostNode, uint32_t generation,
                const PlaybackClip *clip=nullptr,const PlaybackPcmSource *live=nullptr)
        : stream_(stream), deviceNode_(deviceNode), generation_(generation),
          encoder_(playback[stream < 2 ? stream : 0], hostNode),clip_(clip),live_(live) {
        if (stream >= 2 || deviceNode >= 63 || hostNode >= 63 ||
            (clip && (!clip->valid() || stream!=0)) ||
            (live && (!live->valid() || stream!=0 || clip)))
            fault_ = ReplayError::state;
    }
    ReplayError fault() const { return fault_; }
    size_t queued() const { return count_; }
    size_t playbackFramesPrepared() const {return playbackFrame_;}
    bool ready() const { return fault_ == ReplayError::none && count_ >= Preroll; }

    ReplayError ingest(std::span<const uint8_t> bytes, uint32_t arrival, uint32_t generation) {
        if (fault_ != ReplayError::none) return fault_;
        if (generation != generation_) return fail(ReplayError::generation);
        if (arrival >= cycleStampDomain || (received_ && arrival != nextReceive_))
            return fail(ReplayError::receiveCycle);
        if (count_ == Capacity) return fail(ReplayError::overflow);
        const auto parsed = startup_.parse(bytes, stream_, deviceNode_);
        if (!parsed) return fail(ReplayError::packet);
        uint32_t phaseTicks = 0;
        if (parsed.packet.blocks) {
            const auto value = phase(parsed.packet.syt, arrival % 8000);
            if (!value) return fail(ReplayError::missingSyt);
            phaseTicks = *value;
        }
        if (!continuity_.accept(parsed.packet)) return fail(ReplayError::continuity);
        entries_[(head_ + count_) % Capacity] = {parsed.packet.blocks, phaseTicks};
        ++count_;
        received_ = true;
        nextReceive_ = nextCycleStamp(arrival);
        return ReplayError::none;
    }

    // Output cycle comes from a transport scheduling decision, never a host timer.
    // Preroll is a tunable experiment policy (default four milliseconds), not a
    // device requirement or a guarantee against macOS scheduling stalls.
    ReplayError start(uint32_t firstOutput, uint32_t generation) {
        if (fault_ != ReplayError::none) return fault_;
        if (generation != generation_) return fail(ReplayError::generation);
        if (running_) return fail(ReplayError::state);
        if (firstOutput >= cycleStampDomain) return fail(ReplayError::outputCycle);
        if (!ready()) return ReplayError::state; // Waiting for preroll is not a fault.
        nextOutput_ = firstOutput;
        running_ = true;
        return ReplayError::none;
    }

    ReplayResult produce(std::span<uint8_t> output, uint32_t cycle, uint32_t generation) {
        if (fault_ != ReplayError::none) return {0, fault_};
        if (generation != generation_) return {0, fail(ReplayError::generation)};
        if (!running_) return {0, ReplayError::state};
        if (cycle >= cycleStampDomain || cycle != nextOutput_)
            return {0, fail(ReplayError::outputCycle)};
        if (!count_) return {0, fail(ReplayError::underrun)};
        const auto &entry = entries_[head_];
        const uint16_t syt = entry.blocks ? *replaySyt(entry.phase, cycle % 8000) : 0xffff;
        // Preflight before borrowing live PCM: a short destination must be a
        // safe retry even if the producer is late or has failed in the meantime.
        if(output.size()<8+size_t(entry.blocks)*playback[stream_].slots()*4)return {0,ReplayError::capacity};
        std::array<int32_t,8*12> samples{};
        std::optional<size_t> size;
        if(live_ && entry.blocks) {
            auto view=std::span(samples).first(size_t(entry.blocks)*live_->channels);
            if(!live_->read(live_->context,playbackFrame_,view))return {0,fail(ReplayError::source)};
            size=encoder_.makeInterleaved(output,entry.blocks,syt,view,live_->channels);
            if(!size || !live_->commit(live_->context,playbackFrame_,entry.blocks))return {0,fail(ReplayError::source)};
        } else size=encoder_.make(output,entry.blocks,syt,clip_,playbackFrame_);
        if (!size) return {0, ReplayError::capacity}; // Retry same cycle without consuming.
        if(live_ && live_->prepared && entry.blocks)
            live_->prepared(live_->context,{playbackFrame_,preparedPackets_,cycle,syt,entry.blocks});
        // Only accepted data packets consume PCM. No-data and failed capacity
        // retries leave the source position unchanged. Only finite clips pad EOF;
        // a missing live interval is a terminal source fault.
        playbackFrame_+=entry.blocks;
        ++preparedPackets_;
        head_ = (head_ + 1) % Capacity;
        --count_;
        nextOutput_ = nextCycleStamp(cycle);
        return {*size, ReplayError::none};
    }
};
} // namespace Pro40Packets
