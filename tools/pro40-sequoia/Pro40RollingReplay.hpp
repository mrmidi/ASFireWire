// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativePacketBuffer.hpp"
#include "Pro40NativeReplay.hpp"

namespace Pro40Packets {
// Content composition only: one serialized owner, no DMA, allocation or I/O.
// Linux amdtp-stream.c:469-625 retains received timing across refills and
// :2015-2046 pairs streams by index. Apple's IOFWUserIsochPort.cpp:1175-1178
// limits a native modification notification to 64 descriptors. No source copied.
enum class RefillResult { waiting, ready, pending, fault };
class RollingReplay {
    std::array<NativeReplay,2> replay_;
    std::array<std::array<NativePackets::PreparedBuffer,64>,2> packets_{};
    const uint32_t generation_;
    uint32_t nextOutput_;
    uint64_t nextSource_=0;
    bool started_=false,pending_=false,failed_=false;
    RefillResult fail() { failed_=true;pending_=false;return RefillResult::fault; }
public:
    RollingReplay(const std::array<uint8_t,2> &channels,uint8_t remote,
                  uint8_t local,uint32_t generation,uint32_t firstOutput)
        : replay_{NativeReplay(0,channels[0],remote,local,generation),
                  NativeReplay(1,channels[1],remote,local,generation)},
          generation_(generation),nextOutput_(firstOutput) {
        failed_=firstOutput>=cycleStampDomain || channels[0]>=63 || channels[1]>=63;
    }
    uint64_t sourceOffset() const { return nextSource_; }
    bool fault() const { return failed_; }

    // completed[] means consecutive, readable slots from index zero, never a
    // count of arbitrary callbacks. Source views must remain valid during this
    // call. A transport watchdog must independently bound elapsed wall time.
    template<class Source> RefillResult prepare(Source source,
        const std::array<uint64_t,2> &completed,uint32_t generation) {
        if(failed_ || generation!=generation_)return fail();
        if(pending_)return RefillResult::pending; // Never overwrite an unacknowledged pair.
        for(const auto count:completed)if(count<nextSource_)return fail();
        for(const auto count:completed)if(count-nextSource_<64)return RefillResult::waiting;
        for(unsigned stream=0;stream<2;++stream) {
            for(unsigned n=0;n<64;++n) {
                const auto packet=source(stream,nextSource_+n);
                if(replay_[stream].ingest(packet.bytes,packet.status,packet.timestamp,generation)
                    !=NativeReplayError::none)return fail();
            }
            if(!started_ && replay_[stream].start(nextOutput_,generation)!=ReplayError::none)
                return fail();
            for(unsigned n=0;n<64;++n) {
                auto &packet=packets_[stream][n];
                packet={};
                const auto result=replay_[stream].produce(packet.bytes,
                    (nextOutput_+n)%cycleStampDomain,generation);
                if(!result)return fail();
                packet.length=uint32_t(result.bytes);
            }
        }
        started_=true;pending_=true;return RefillResult::ready;
    }
    // Borrowed only until acknowledgement. The transport must copy BOTH batches
    // into its own lifetime-owned mappings before acknowledging success. This
    // does not make two native notifications atomic: partial publication must
    // acknowledge failure and stop the session, never retry the surviving half.
    std::span<const NativePackets::PreparedBuffer> batch(unsigned stream) const {
        if(failed_ || !pending_ || stream>=2)return {};
        return packets_[stream];
    }
    bool acknowledge(uint64_t offset,bool success) {
        if(failed_ || !pending_ || offset!=nextSource_ || !success) {
            fail();return false;
        }
        nextSource_+=64;nextOutput_=(nextOutput_+64)%cycleStampDomain;
        pending_=false;return true;
    }
};
}
