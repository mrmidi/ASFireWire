// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativePacketBuffer.hpp"
#include "NativeReceivePacket.hpp"
#include "InputMonitor.hpp"
#include "SustainedInputHandoff.hpp"

namespace Pro40Audio {
enum class NativeInputDeliveryFault { none, source, transport, framing, sequence };
// One native event-loop owner. Decode already-owned receive records independently
// of the playback sequence consumer. Linux amdtp-stream.c:1330-1347 processes
// received payloads before caching their sequence for replay; AM824 PCM channel
// extraction is checked against amdtp-am824.c:179-210. No wire scheduling changes.
class NativeInputDelivery {
public:
    // One fault-only content record, read on the owner after native Stop.
    // No PCM values, native memory views, allocation, or hot-path logging.
    struct Rejection {
        bool present=false;
        uint32_t q0=0,q1=0,previousQ0=0,previousQ1=0,arrival=0,previousArrival=0;
        uint32_t size=0,parseError=0,handoffFault=0,badPcmLabels=0,nonzeroInvalidPcm=0;
    };
private:
    const unsigned channel_;
    const uint8_t sender_;
    const uint32_t generation_;
    Pro40Packets::InputMonitor &monitor_;
    SustainedInputHandoff *handoff_;
    uint64_t next_=0;
    unsigned maximumBatch_=0;
    NativeInputDeliveryFault fault_=NativeInputDeliveryFault::none;
    NativePackets::Error transport_=NativePackets::Error::none;
    Rejection rejection_{};
    Pro40Packets::CaptureStartup startup_;
    uint32_t previousQ0_=0,previousQ1_=0,previousArrival_=0;
    bool fail(NativeInputDeliveryFault fault) {fault_=fault;return false;}
    bool reject(std::span<const uint8_t> payload,uint32_t arrival) {
        rejection_.present=true;rejection_.size=uint32_t(payload.size());
        rejection_.arrival=arrival;rejection_.previousArrival=previousArrival_;
        rejection_.previousQ0=previousQ0_;rejection_.previousQ1=previousQ1_;
        if(payload.size()>=8) {
            rejection_.q0=Pro40Packets::read32(payload.data());
            rejection_.q1=Pro40Packets::read32(payload.data()+4);
        }
        rejection_.parseError=unsigned(Pro40Packets::parseCaptureTiming(payload,0,sender_).error);
        rejection_.handoffFault=handoff_?unsigned(handoff_->fault()):0;
        if(payload.size()==360)for(unsigned f=0;f<8;++f)for(unsigned c=0;c<8;++c) {
            const auto word=Pro40Packets::read32(payload.data()+8+44*f+4*c);
            if((word>>24)!=0x40){++rejection_.badPcmLabels;rejection_.nonzeroInvalidPcm+=word!=0;}
        }
        return fail(NativeInputDeliveryFault::sequence);
    }
public:
    // At most one TX bank's work per loop. This also guarantees that the next
    // TX acknowledgement cannot outrun input delivery, even after a backlog.
    static constexpr unsigned budget=256;
    NativeInputDelivery(unsigned channel,uint8_t sender,uint32_t generation,
                        Pro40Packets::InputMonitor &monitor,SustainedInputHandoff *handoff)
        :channel_(channel),sender_(sender),generation_(generation),monitor_(monitor),handoff_(handoff) {}
    uint64_t next() const {return next_;}
    unsigned maximumBatch() const {return maximumBatch_;}
    NativeInputDeliveryFault fault() const {return fault_;}
    NativePackets::Error transportError() const {return transport_;}
    const Rejection &rejection() const {return rejection_;}
    const auto &startup() const {return handoff_?handoff_->startup():startup_;}
    template<class Copy> bool drain(uint64_t available,uint32_t generation,Copy copy) {
        if(fault_!=NativeInputDeliveryFault::none)return false;
        if(generation!=generation_ || available<next_ || available%64 || next_%64)
            return fail(NativeInputDeliveryFault::source);
        const auto count=unsigned(std::min<uint64_t>(available-next_,budget));
        maximumBatch_=std::max(maximumBatch_,count);
        NativePackets::PreparedBuffer owned;uint32_t status=0,stamp=0;
        for(unsigned n=0;n<count;++n) {
            if(!copy(next_,owned,status,stamp))return fail(NativeInputDeliveryFault::source);
            const auto packet=NativePackets::receive(owned.bytes,status,stamp,channel_);
            transport_=packet.error;
            if(!packet)return fail(NativeInputDeliveryFault::transport);
            if(packet.tag!=1 || packet.sync!=0)return fail(NativeInputDeliveryFault::framing);
            if(!handoff_ && !startup_.parse(packet.payload,0,sender_))return reject(packet.payload,packet.cycleStamp);
            if(handoff_ && !handoff_->packet(packet.payload,packet.cycleStamp,generation,sender_))
                return reject(packet.payload,packet.cycleStamp);
            if(!monitor_.observe(packet.payload,startup().preroll()))return reject(packet.payload,packet.cycleStamp);
            if(packet.payload.size()>=8) {
                previousQ0_=Pro40Packets::read32(packet.payload.data());
                previousQ1_=Pro40Packets::read32(packet.payload.data()+4);
            }
            previousArrival_=packet.cycleStamp;
            ++next_;
        }
        return true;
    }
};
}
