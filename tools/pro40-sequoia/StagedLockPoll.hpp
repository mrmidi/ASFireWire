// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
namespace Pro40Packets {
// A single content-side observation, advanced by the event-loop owner only
// after servicing pending DMA work. One operation per advance: three distinct
// quadlet reads (clock/extended/rate), then the cycle timer. Quadlet semantics
// matter: Linux dice-interface.h:134-168 specifies read-to-clear slip bits.
// No native pointers, allocation, automatic retry, or publication here.
class StagedLockPoll {
public:
    static constexpr unsigned maximumSpanCycles=256;
    enum class State { pending, complete, cancelled, failed };
    enum class Error { none, generation, progress, age, io };
    struct Result {
        bool ready=true;
        int32_t status=0;
        Result(int32_t s):status(s) {}
        static Result pending() {Result r(0);r.ready=false;return r;}
    };
    struct Sample {
        unsigned started=0, completed=0, attempted=0, valid=0;
        std::array<unsigned,4> progress{};
        std::array<uint32_t,4> values{}; // clock, extended status, rate, timer
        int32_t ioStatus=0;
    };
private:
    const uint32_t generation_;
    const unsigned budget_;
    const unsigned previousCompletion_;
    unsigned next_=0, previous_=0;
    bool began_=false;
    State state_=State::pending;
    Error error_=Error::none;
    Sample sample_{};
    State fail(Error error) {error_=error;return state_=State::failed;}
public:
    StagedLockPoll(uint32_t generation,unsigned budget,unsigned previousCompletion=0):
        generation_(generation),budget_(budget),previousCompletion_(previousCompletion) {}
    State state() const {return state_;}
    Error error() const {return error_;}
    const Sample &sample() const {return sample_;}
    // Operation(index, value) returns a status or Result::pending(). Pending
    // operations are polled again without advancing or accepting partial data. Index3
    // is the timer query; never combine the preceding quadlets into a block.
    // Stop/generation/age validation runs even when buffer work defers polling.
    template<class Operation>
    State advance(unsigned progress,uint32_t generation,bool stop,bool deferred,Operation operation) {
        if(state_!=State::pending)return state_;
        if(stop)return state_=State::cancelled;
        if(generation!=generation_)return fail(Error::generation);
        if(previousCompletion_>=budget_ || progress<128 || progress>=budget_ ||
            progress<previousCompletion_ || (began_ && progress<previous_))return fail(Error::progress);
        if(began_ && progress-sample_.started>maximumSpanCycles)return fail(Error::age);
        previous_=progress;
        if(deferred)return state_;
        // The next query may begin at the previous completed watermark rather
        // than wait another64-packet notification. Its final timestamp still
        // requires strictly advancing progress, with all age/reset/stop checks
        // above running while waiting. Never reread the read-to-clear quadlets.
        if(next_==3 && progress<=previousCompletion_)return state_;
        if(!began_){began_=true;sample_.started=progress;}
        if(!(sample_.attempted&(1u<<next_)))sample_.progress[next_]=progress;
        sample_.attempted|=1u<<next_;
        uint32_t value=0;
        const Result result=operation(next_,value);
        if(!result.ready)return state_;
        sample_.ioStatus=result.status;
        if(sample_.ioStatus)return fail(Error::io);
        sample_.values[next_]=value;
        sample_.valid|=1u<<next_;
        if(++next_==4){sample_.completed=progress;state_=State::complete;}
        return state_;
    }
};
}
