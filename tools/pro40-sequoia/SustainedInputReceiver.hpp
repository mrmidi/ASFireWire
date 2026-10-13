// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SustainedInputHandoff.hpp"
#include "SustainedClockFit.hpp"
#include "AudioFeedSession.hpp"
#include <optional>

namespace Pro40Audio {
// Non-realtime owner: validates clock events in exactly the consumed PCM order,
// assembles existing P4F0 eight-channel batches and publishes immutable windows.
// Readers use the feed's existing pin/lease contract. No hardware access or
// reference to the finite recorder. A pending publication holds back dequeue;
// persistent backpressure faults the native producer instead of losing frames.
// Clock snapshots here are DIAGNOSTIC: a later start/update contract must gate
// them against retained/published PCM and fit quality before HAL can use them.
template<size_t QueueCapacity=2048,size_t WindowFrames=4096> class SustainedInputReceiver {
    BasicSustainedInputHandoff<QueueCapacity> &source_;
    AudioFeedSession<WindowFrames,8> feed_;
    std::optional<SustainedClockFit> fit_;
    AudioFeedBatchFor<8> batch_;
    std::array<uint8_t,AudioFeedPacketFor<8>::maxBytes> bytes_{};
    uint64_t packets_=0,frames_=0,clockPairs_=0,snapshots_=0,snapshotFrames_=0;
    bool ended_=false,ending_=false,failed_=false;
    NativeClockSnapshot snapshot_{};
    bool prerollSeen_=false;
    bool fail(SustainedInputFault why) noexcept {
        source_.fail(why);feed_.close();failed_=true;return false;
    }
    bool publish() noexcept {
        if(!batch_.frames)return true;
        size_t used=0;
        if(AudioFeedPacketFor<8>::encode(batch_,bytes_,used)!=AudioFeedError::ok)
            return fail(SustainedInputFault::consumer);
        const auto result=feed_.receive(std::span(bytes_).first(used));
        if(result!=SessionResult::ok && result!=SessionResult::pending)return fail(SustainedInputFault::consumer);
        batch_.firstFrame+=batch_.frames;batch_.frames=0;return true;
    }
public:
    SustainedInputReceiver(BasicSustainedInputHandoff<QueueCapacity> &source,uint64_t epoch)
        :source_(source),feed_(epoch,Pro40Packets::CaptureStartup::discardedFrames) {
        batch_.epoch=epoch;batch_.firstFrame=Pro40Packets::CaptureStartup::discardedFrames;
    }
    auto tryAcquire() noexcept {return feed_.tryAcquire();}
    bool ended() const noexcept {return ended_;} // Owner or after join only below.
    bool failed() const noexcept {return failed_;}
    uint64_t frames() const noexcept {return frames_;}
    uint64_t packets() const noexcept {return packets_;}
    uint64_t clockPairs() const noexcept {return clockPairs_;}
    uint64_t publishedNext() const noexcept {return feed_.publishedNext();}
    uint64_t snapshots() const noexcept {return snapshots_;}
    uint64_t snapshotFrames() const noexcept {return snapshotFrames_;}
    bool snapshot(NativeClockSnapshot &out) const noexcept {
        if(failed_ || !snapshots_)return false;
        out=snapshot_;return true;
    }
    // Bounded amount of work per poll. Caller controls sleep/wake and deadline;
    // no blocking or thread creation hidden in this object. False is terminal.
    bool poll(size_t budget=256) noexcept {
        if(source_.fault()!=SustainedInputFault::none || failed_)return fail(SustainedInputFault::consumer);
        if(ended_)return true;
        if(feed_.flush()==SessionResult::pending)return true;
        if(feed_.closed())return fail(SustainedInputFault::consumer);
        if(ending_){ended_=true;return true;}
        SustainedInputEvent event;
        while(budget-- && source_.pop(event)) {
            if(event.kind==SustainedInputEvent::Kind::clock) {
                if(!fit_) {
                    if(!event.clock.beforeReceive || packets_)return fail(SustainedInputFault::clock);
                    fit_.emplace(event.clock.generation,source_.numer,source_.denom);
                }
                if(!fit_->clock(event.clock))return fail(SustainedInputFault::clock);
                ++clockPairs_;
                if(fit_->snapshot(snapshot_)){++snapshots_;snapshotFrames_=frames_;}
            } else if(event.kind==SustainedInputEvent::Kind::packet || event.kind==SustainedInputEvent::Kind::preroll) {
                if(!fit_ || !fit_->packet(event.packet))return fail(SustainedInputFault::packet);
                if(event.kind==SustainedInputEvent::Kind::preroll) {
                    if(prerollSeen_ || frames_ || event.packet.blocks!=Pro40Packets::CaptureStartup::discardedFrames)
                        return fail(SustainedInputFault::packet);
                    prerollSeen_=true; // Deliberately absent from the feed, never substituted with silence.
                } else {
                    if(event.packet.blocks && !prerollSeen_)return fail(SustainedInputFault::packet);
                    for(size_t n=0;n<event.packet.blocks*8;++n)batch_.samples[batch_.frames*8+n]=event.samples[n];
                    batch_.frames+=event.packet.blocks;
                }
                frames_+=event.packet.blocks;++packets_;
                // Preserve absolute 512-frame publication boundaries: the first
                // batch has 504 usable frames [8,512), subsequent batches 512.
                // Clock evidence/admission must not wait forever for a prefix
                // that an eight-frame-shifted publication could never cover.
                if((batch_.firstFrame+batch_.frames)%batch_.maxFrames==0 && !publish())return false;
            } else {
                if(!fit_ || event.packet.ordinal!=packets_ || event.packet.firstFrame!=frames_ ||
                   frames_<=Pro40Packets::CaptureStartup::discardedFrames)
                    return fail(SustainedInputFault::consumer);
                if(!publish())return false;
                ending_=true;
            }
            if(feed_.pending())return true;
            if(ending_){ended_=true;return true;}
        }
        if(source_.fault()!=SustainedInputFault::none)return fail(SustainedInputFault::consumer);
        return true;
    }
};
}
