// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioSessionGate.hpp"
#include "AudioSyntheticReceiver.hpp"
#include <new>
namespace Pro40Audio {
class AudioSyntheticSession {
    friend class AudioSyntheticSessionControl;
    const AudioSyntheticStart clock_;
    mutable AudioSyntheticReceiver receiver_;
    explicit AudioSyntheticSession(AudioSyntheticStart start):clock_(start) {}
public:
    const AudioSyntheticStart &clock() const noexcept {return clock_;}
    bool sourceFinished() const noexcept {return receiver_.report().has_value();}
    CallbackSessionResult read(const AudioSyntheticStart &expected,const AudioServerPlugInIOCycleInfo &cycle,
                               UInt32 frames,std::span<float> out) const noexcept {
        if(expected!=clock_) {std::fill(out.begin(),out.end(),0);return {};}
        return receiver_.readInput(clock_.epoch,clock_.seed,cycle,frames,out);
    }
};
enum class SyntheticControlResult {ok, busy, invalidClock, preparationFailed, cleanupFailed, allocationFailed};
// One serialized control owner. Readers only acquire leases. No host notification
// or client registry here. This object/gate must outlive ALL reader attempts.
class AudioSyntheticSessionControl {
    enum class State {idle,running,draining,blocked};
    State state_=State::idle;
    AudioSessionGate<AudioSyntheticSession> gate_;
    uint64_t lastSeed_=0,lastEpoch_=0;
public:
    auto tryAcquire() noexcept {return gate_.tryAcquire();}
    SyntheticControlResult start(mach_port_t control,const AudioSyntheticStart &clock) {
        if(state_==State::blocked)return SyntheticControlResult::cleanupFailed;
        if(state_!=State::idle)return SyntheticControlResult::busy;
        if(!clock.valid() || clock.seed<=lastSeed_ || clock.epoch==lastEpoch_)return SyntheticControlResult::invalidClock;
        auto prepared=std::unique_ptr<AudioSyntheticSession>(new(std::nothrow) AudioSyntheticSession(clock));
        if(!prepared)return SyntheticControlResult::allocationFailed;
        const auto result=prepared->receiver_.prepare(control,clock);
        if(result.result!=SyntheticPrepare::ok) {
            if(!result.cleanupOK){state_=State::blocked;return SyntheticControlResult::cleanupFailed;}
            return SyntheticControlResult::preparationFailed;
        }
        // No clock or receiver is externally visible until the single gate
        // publication. Last IDs advance only on successful publication.
        if(!gate_.publish(prepared)) {
            const bool clean=prepared->receiver_.stopAndJoin();
            if(!clean){state_=State::blocked;return SyntheticControlResult::cleanupFailed;}
            return SyntheticControlResult::busy;
        }
        lastSeed_=clock.seed;lastEpoch_=clock.epoch;state_=State::running;return SyntheticControlResult::ok;
    }
    SyntheticControlResult stop() {
        if(state_==State::blocked)return SyntheticControlResult::cleanupFailed;
        if(state_==State::idle)return SyntheticControlResult::ok;
        gate_.closeAdmission();state_=State::draining;
        auto retired=gate_.tryReclaim();
        if(!retired)return SyntheticControlResult::busy; // Keep allocation; never free pinned data.
        if(!retired->receiver_.stopAndJoin()){state_=State::blocked;return SyntheticControlResult::cleanupFailed;}
        state_=State::idle;return SyntheticControlResult::ok;
    }
    // Owner must close, quiesce external callers and finish stop before normal
    // destruction. Member teardown is a final worker join, not callback quiescence.
};
}
