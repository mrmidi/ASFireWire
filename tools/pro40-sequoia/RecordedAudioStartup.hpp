// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "InputRecording.hpp"
#include "InputTimingEvidence.hpp"
#include "NativeAudioStartup.hpp"
namespace Pro40Audio {
// Content-side adapter, serialized on the capture owner. The recorder must append
// first, then deliver exactly one timing record (including empty packets).
// Both referenced objects must outlive this adapter. No observer installation,
// transport parsing, allocation, or IO occurs here. The caller must propagate
// recorder faults even when a timing record is not appended (poll/complete check).
template<size_t Capacity> class RecordedAudioStartup {
public:
    RecordedAudioStartup(NativeAudioStartup<Capacity> &owner,
                         const Pro40Packets::InputRecording &recording,uint64_t epoch)
        :owner_(owner),recording_(recording),epoch_(epoch){
        if(!epoch_||!unchanged())failed();
    }
    RecordedAudioStartup(const RecordedAudioStartup &)=delete;
    RecordedAudioStartup &operator=(const RecordedAudioStartup &)=delete;
    bool record(size_t ordinal,const Pro40Packets::InputTimingEvidence<>::Record &r) noexcept {
        if(blocked_||!recording_.valid()||recording_.fault()||ordinal!=packets_||
           r.firstFrame!=frames_||(r.blocks!=0&&r.blocks!=8))return failed();
        const auto bytes=recording_.pcm();
        // Division avoids overflow when comparing externally supplied counts.
        if(bytes.size()%3||bytes.size()/3!=frames_+r.blocks)return failed();
        InputBlock block{epoch_,frames_,{}};
        if(r.blocks){
            const auto tail=bytes.last(24);
            for(size_t n=0;n<8;++n){
                const uint32_t bits=uint32_t(tail[n*3])|(uint32_t(tail[n*3+1])<<8)|
                                    (uint32_t(tail[n*3+2])<<16);
                block.inputOne[n]=static_cast<int32_t>(bits)-((bits&0x800000)?0x1000000:0);
            }
        }
        if(!owner_.packet({ordinal,r.firstFrame,r.generation,r.arrivalCycle,r.syt,r.blocks,r.dbc},
                          r.blocks?&block:nullptr))return failed();
        ++packets_;frames_+=r.blocks;return true;
    }
    bool clock(const ControllerClockFit::Pair &p,uint64_t now) noexcept {
        return unchanged()&&owner_.clock(p,now)?true:failed();
    }
    bool poll(uint64_t now) noexcept {return unchanged()&&owner_.poll(now)?true:failed();}
    bool complete(uint64_t now,std::span<int32_t> output,SourceFrameClock::Stamp &stamp) noexcept {
        std::fill(output.begin(),output.end(),0);
        return unchanged()&&owner_.complete(now,output,stamp)?true:failed();
    }
    bool failed() noexcept {blocked_=true;owner_.stop();return false;}
private:
    bool unchanged() const noexcept {
        return !blocked_&&recording_.valid()&&!recording_.fault()&&
               recording_.pcm().size()%3==0&&recording_.pcm().size()/3==frames_;
    }
    NativeAudioStartup<Capacity> &owner_;
    const Pro40Packets::InputRecording &recording_;
    uint64_t epoch_,frames_=0,packets_=0;bool blocked_=false;
};
}
