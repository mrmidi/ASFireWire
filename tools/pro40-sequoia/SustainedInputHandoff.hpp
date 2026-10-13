// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ControllerClockFit.hpp"
#include "PacketClockFit.hpp"
#include "Pro40CaptureStartup.hpp"
#include <atomic>

namespace Pro40Audio {
enum class SustainedInputFault : uint32_t {none, state, packet, full, clock, consumer};
struct SustainedInputEvent {
    enum class Kind {clock, preroll, packet, end} kind=Kind::clock;
    ControllerClockPair clock{};
    PacketClockRecord packet{};
    std::array<int32_t,8*8> samples{};
};
// Owned content seam, one native producer and one non-RT consumer. Payload and
// metadata are copied before native buffers can be reused. No DMA pointers,
// heap allocation, logging, IPC, waiting or fit calculation on the producer.
// Queue exhaustion is terminal: never hide a dropped packet or reset ordinals.
// Construct/touch before native entry. Stop native callbacks, then finish the
// producer, join the consumer and quiesce feed readers before destroying it.
template<size_t Capacity=2048> class BasicSustainedInputHandoff {
    static_assert(Capacity>=2);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<SustainedInputFault>::is_always_lock_free);
    std::array<SustainedInputEvent,Capacity> queue_{};
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
    std::atomic<SustainedInputFault> fault_{SustainedInputFault::none};
    // Remaining fields are producer-only, immutable, or read after join.
    uint64_t packets_=0,frames_=0,highWater_=0;
    uint32_t generation_=0,lastArrival_=0;
    Pro40Packets::Continuity continuity_;
    Pro40Packets::CaptureStartup startup_;
    bool anchored_=false,finished_=false;
    bool push(const SustainedInputEvent &event) noexcept {
        if(fault()!=SustainedInputFault::none)return false;
        const auto write=write_.load(std::memory_order_relaxed);
        const auto read=read_.load(std::memory_order_acquire);
        if(write-read>=Capacity)return fail(SustainedInputFault::full);
        queue_[write%Capacity]=event;
        if(write-read+1>highWater_)highWater_=write-read+1;
        write_.store(write+1,std::memory_order_release);return true;
    }
public:
    static constexpr uint64_t packetLimit=691200000; // Native counter guard: 24 hours.
    const uint32_t numer,denom;
    BasicSustainedInputHandoff(uint32_t n,uint32_t d):numer(n),denom(d) {
        if(!n || !d)fail(SustainedInputFault::state);
    }
    BasicSustainedInputHandoff(const BasicSustainedInputHandoff &)=delete;
    bool fail(SustainedInputFault value) noexcept {
        auto expected=SustainedInputFault::none;
        fault_.compare_exchange_strong(expected,value,std::memory_order_acq_rel);
        return false;
    }
    SustainedInputFault fault() const noexcept {return fault_.load(std::memory_order_acquire);}
    bool fresh() const noexcept { // Before producer/consumer entry only.
        return fault()==SustainedInputFault::none && !anchored_ && !finished_ && !packets_ && !write_.load();
    }
    uint32_t packetCount() const noexcept {return uint32_t(packets_);} // Producer only.
    uint64_t frames() const noexcept {return frames_;} // Producer or after join.
    uint64_t highWater() const noexcept {return highWater_;} // Producer or after join.
    const auto &startup() const noexcept {return startup_;} // Producer or after join.
    bool clock(ControllerClockPair row) noexcept {
        if(finished_ || row.packets!=packets_ || row.status || !row.uptime ||
           row.before>row.uptime || row.uptime>row.after ||
           (anchored_ ? (row.beforeReceive || row.generation!=generation_) : (!row.beforeReceive || packets_)))
            return fail(SustainedInputFault::clock);
        SustainedInputEvent event;event.kind=SustainedInputEvent::Kind::clock;event.clock=row;
        if(!push(event))return false;
        generation_=row.generation;anchored_=true;return true;
    }
    bool packet(std::span<const uint8_t> payload,uint32_t arrival,uint32_t generation,uint8_t sender) noexcept {
        if(fault()!=SustainedInputFault::none)return false;
        if(!anchored_ || finished_)return fail(SustainedInputFault::state);
        const auto parsed=startup_.parse(payload,0,sender);
        if(!parsed || packets_>=packetLimit || generation!=generation_ || arrival>=64000 ||
           (packets_ && arrival!=(lastArrival_+1)%64000) ||
           (parsed.packet.blocks && parsed.packet.syt==0xffff) || !continuity_.accept(parsed.packet))
            return fail(SustainedInputFault::packet);
        SustainedInputEvent event;event.kind=SustainedInputEvent::Kind::packet;
        event.packet={packets_,frames_,generation,arrival,parsed.packet.syt,parsed.packet.blocks,parsed.packet.dbc};
        if(startup_.preroll())event.kind=SustainedInputEvent::Kind::preroll;
        // References: linux-dice/amdtp-am824.c:99-103,179-207 describes PCM
        // slot order; focused SPro40 map in protocols/dice/src/focusrite/spro40.rs.
        // First eight analog slots only; require valid PCM labels for live input.
        for(unsigned f=0;!startup_.preroll() && f<parsed.packet.blocks;++f)for(unsigned c=0;c<8;++c) {
            const auto sample=Pro40Packets::sample24(payload.subspan(8+44*f+4*c,4));
            if(!sample)return fail(SustainedInputFault::packet);
            event.samples[f*8+c]=*sample;
        }
        if(!push(event))return false;
        lastArrival_=arrival;frames_+=parsed.packet.blocks;++packets_;return true;
    }
    // Only after native teardown. A rejected native run must call fail instead.
    bool finish() noexcept {
        if(!anchored_ || finished_)return fail(SustainedInputFault::state);
        SustainedInputEvent event;event.kind=SustainedInputEvent::Kind::end;
        event.packet.ordinal=packets_;event.packet.firstFrame=frames_;
        if(!push(event))return false;
        finished_=true;return true;
    }
    bool pop(SustainedInputEvent &out) noexcept { // Consumer only.
        if(fault()!=SustainedInputFault::none)return false;
        const auto read=read_.load(std::memory_order_relaxed);
        if(read==write_.load(std::memory_order_acquire))return false;
        out=queue_[read%Capacity];read_.store(read+1,std::memory_order_release);return true;
    }
};
using SustainedInputHandoff=BasicSustainedInputHandoff<>;
}
