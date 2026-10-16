// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PlaybackPcmQueue.hpp"
namespace Pro40Audio {
enum class PlaybackStartupFault:uint32_t {none,plan,late,unprimed,sequence,source};
// Content-side adapter, never a DMA or clock controller. One connection owner
// schedules/arms; the native replay owner calls read/commit. The queue starts at
// application frame0. A single immutable native frame boundary maps to that0.
// Before it, transmit labelled PCM silence through the EXISTING AM824 encoder.
// After it, missing PCM is terminal; never insert silence or slide the boundary.
// PCM silence representation: references/linux-dice/amdtp-am824.c:209-221.
// Existing sequence replay: amdtp-stream.c:469-530. No wire code copied.
class PlaybackStartupGate final {
    PlaybackPcmQueue<> &queue_;
    std::atomic<uint64_t> boundary_{UINT64_MAX},next_{0},startedAt_{UINT64_MAX};
    std::atomic<bool> armed_{false};
    std::atomic<PlaybackStartupFault> fault_{PlaybackStartupFault::none};
    uint64_t prime_=0; // Written once before boundary release; native acquires it.
    bool scheduled_=false; // Connection owner only.
    bool pending_=false,pendingAudio_=false; // Native owner only.
    uint64_t pendingFirst_=0,pendingQueueFirst_=0;
    std::array<Pro40Packets::PlaybackPacketPosition,3> positions_{}; // Native owner; all3 inspected after teardown. Published0 is immutable.
    std::atomic<bool> firstPositionReady_{false}; // positions_[0] becomes immutable before release.
    bool fail(PlaybackStartupFault why)noexcept {
        auto ok=PlaybackStartupFault::none;fault_.compare_exchange_strong(ok,why,std::memory_order_acq_rel);return false;
    }
public:
    explicit PlaybackStartupGate(PlaybackPcmQueue<> &queue):queue_(queue) {
        if(queue.consumed() || queue.written() || queue.fault()!=PlaybackQueueResult::ok)fail(PlaybackStartupFault::plan);
    }
    PlaybackStartupGate(const PlaybackStartupGate &)=delete;
    PlaybackStartupFault fault()const noexcept{return fault_.load(std::memory_order_acquire);}
    uint64_t nextNativeFrame()const noexcept{return next_.load(std::memory_order_acquire);}
    uint64_t firstNativeFrame()const noexcept{return boundary_.load(std::memory_order_acquire);}
    uint64_t startedAt()const noexcept{return startedAt_.load(std::memory_order_acquire);}
    bool firstPosition(Pro40Packets::PlaybackPacketPosition &out)const noexcept {
        if(!firstPositionReady_.load(std::memory_order_acquire))return false;
        out=positions_[0];return true; // Owned storage survives bridge/native joins.
    }
    bool schedule(uint64_t first,uint64_t primedFrames)noexcept {
        if(scheduled_ || fault()!=PlaybackStartupFault::none || first==UINT64_MAX || first%8 ||
           !primedFrames || primedFrames>queue_.capacity || primedFrames%8 || first>UINT64_MAX-primedFrames || first<=nextNativeFrame())
            return fail(PlaybackStartupFault::plan);
        scheduled_=true;prime_=primedFrames;boundary_.store(first,std::memory_order_release);
        // A delayed scheduler may lose the race. Do not move the advertised
        // boundary forward. Native read also rejects a missed first frame.
        if(nextNativeFrame()>first)return fail(PlaybackStartupFault::late);
        return true;
    }
    bool arm()noexcept {
        if(!scheduled_ || armed_.load(std::memory_order_relaxed) || fault()!=PlaybackStartupFault::none)
            return fail(PlaybackStartupFault::plan);
        if(queue_.written()<prime_ || queue_.consumed() || queue_.fault()!=PlaybackQueueResult::ok)
            return fail(PlaybackStartupFault::unprimed);
        if(nextNativeFrame()>firstNativeFrame())return fail(PlaybackStartupFault::late);
        armed_.store(true,std::memory_order_release);return fault()==PlaybackStartupFault::none;
    }
    bool read(uint64_t first,std::span<int32_t> output)noexcept {
        if(fault()!=PlaybackStartupFault::none)return false;
        if(output.size()!=8*6 || first!=nextNativeFrame() || first>UINT64_MAX-8)
            return fail(PlaybackStartupFault::sequence);
        if(!pending_) {
            const auto boundary=firstNativeFrame();
            pendingAudio_=boundary!=UINT64_MAX && first>=boundary;
            if(pendingAudio_) {
                if(!armed_.load(std::memory_order_acquire))return fail(PlaybackStartupFault::unprimed);
                const auto began=startedAt();
                if((began==UINT64_MAX && first!=boundary) || (began!=UINT64_MAX && began!=boundary))return fail(PlaybackStartupFault::late);
                pendingQueueFirst_=first-boundary;
            }
            pendingFirst_=first;pending_=true;
        } else if(pendingFirst_!=first)return fail(PlaybackStartupFault::sequence);
        if(pendingAudio_) {
            if(!queue_.read(pendingQueueFirst_,output))return fail(PlaybackStartupFault::source);
        } else std::fill(output.begin(),output.end(),0);
        return true;
    }
    bool commit(uint64_t first,unsigned frames)noexcept {
        if(fault()!=PlaybackStartupFault::none)return false;
        if(!pending_ || first!=pendingFirst_ || first!=nextNativeFrame() || frames!=8)return fail(PlaybackStartupFault::sequence);
        if(pendingAudio_) {
            if(!queue_.commit(pendingQueueFirst_,frames))return fail(PlaybackStartupFault::source);
            if(startedAt()==UINT64_MAX)startedAt_.store(first,std::memory_order_release);
        }
        pending_=false;next_.store(first+frames,std::memory_order_release);return true;
    }
    Pro40Packets::PlaybackPcmSource port()noexcept {
        return {this,6,[](void *p,uint64_t f,std::span<int32_t> s)noexcept{return static_cast<PlaybackStartupGate *>(p)->read(f,s);},
            [](void *p,uint64_t f,unsigned n)noexcept{return static_cast<PlaybackStartupGate *>(p)->commit(f,n);},
            [](void *p,const Pro40Packets::PlaybackPacketPosition &position)noexcept{
                auto &self=*static_cast<PlaybackStartupGate *>(p);
                if(!self.firstPositionReady_.load(std::memory_order_relaxed)) {
                    self.positions_[0]=position;
                    self.firstPositionReady_.store(true,std::memory_order_release);
                }
                const auto boundary=self.firstNativeFrame();
                if(boundary!=UINT64_MAX) {
                    if(position.frame==boundary)self.positions_[1]=position;
                    if(position.frame==boundary+self.prime_)self.positions_[2]=position;
                }
            }};
    }
    const auto &positions()const noexcept{return positions_;} // Native owner quiesced.
};
}
