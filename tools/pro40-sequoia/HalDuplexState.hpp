// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioSessionGate.hpp"
#include "HalSilenceState.hpp"
#include "SourceFrameClock.hpp"
#include <CoreAudio/AudioServerPlugIn.h>
#include <algorithm>
#include <span>
namespace Pro40Audio {
// Device-wide IO holds follow the installed-host lifecycle observations in
// HalSilenceModel, but timestamps always come from the native session. No nominal
// clock fallback. Control calls serialize; host notifications happen AFTER they
// return. Callback calls never take the mutex or join a worker.
//
// Session owns DuplexSessionBuffers plus its connection worker. stopAndJoin MUST
// join all buffer/connection users, even when returning false for resource-close
// failure. requestStop is concurrent. Reclamation/destruction is control-only.
template<class Session> class HalDuplexState {
    HalSilenceState clients_;
    AudioSessionGate<Session> gate_;
    std::mutex control_;
    std::atomic_flag outputBusy_=ATOMIC_FLAG_INIT;
    bool stopping_=false,blocked_=false,sourceLost_=false;
    uint64_t previousEpoch_=0;
    using Lease=typename AudioSessionGate<Session>::Lease;
    bool matches(const Session &session,const HalSilenceModel::Snapshot &s) const noexcept {
        const auto &c=session.descriptor.clock;
        return c.identity.seed==s.seed && c.origin==s.anchor &&
            c.identity.numer==s.numer && c.identity.denom==s.denom;
    }
    std::optional<Lease> active(uint32_t client) noexcept {
        auto pin=gate_.tryAcquire();HalSilenceModel::Snapshot s;
        if(!pin || !clients_.read(s) || !s.active(client))return {};
        if(!matches(pin->get(),s))return {};
        return pin;
    }
    const Session *ownerActive(uint32_t client) const noexcept {
        const auto *session=gate_.ownerSession();HalSilenceModel::Snapshot s;
        return session && clients_.read(s) && s.active(client) && matches(*session,s)?session:nullptr;
    }
    bool drain() {
        if(!stopping_)return !blocked_;
        auto old=gate_.tryReclaim();if(!old)return false;
        if(!old->stopAndJoin())blocked_=true;
        stopping_=false;return !blocked_;
    }
public:
    struct StopResult {bool accepted=false,runningChanged=false,drained=false,cleanupOK=true;};
    HalDuplexState(uint32_t numer,uint32_t denom):clients_(numer,denom) {}
    bool add(uint32_t client){std::lock_guard lock(control_);return clients_.change(HalSilenceState::Change::add,client);}
    bool remove(uint32_t client){std::lock_guard lock(control_);return clients_.change(HalSilenceState::Change::remove,client);}
    bool snapshot(HalSilenceModel::Snapshot &s) const noexcept {return clients_.read(s);}
    // Caller retains and must stop/join rejected preparations. Publication is
    // possible only after startup activation and coherent initial data/clock.
    bool start(uint32_t client,std::unique_ptr<Session> &prepared,uint64_t now) {
        std::lock_guard lock(control_);HalSilenceModel::Snapshot before;
        if(blocked_ || stopping_ || sourceLost_ || !prepared || !clients_.read(before) || before.running() ||
           !before.known(client) || !prepared->ready(now))return false;
        const auto &d=prepared->descriptor;const auto &id=d.clock.identity;
        if(!id.epoch || id.epoch==previousEpoch_ || before.seed==UINT64_MAX || id.seed!=before.seed+1 ||
           id.numer!=before.numer || id.denom!=before.denom)return false;
        if(!clients_.change(HalSilenceState::Change::start,client,d.clock.origin))return false;
        if(!gate_.publish(prepared)) {
            clients_.change(HalSilenceState::Change::stop,client);blocked_=true;return false;
        }
        previousEpoch_=id.epoch;return true;
    }
    bool retainIO(uint32_t client,uint64_t now) {
        std::lock_guard lock(control_);auto *session=ownerActive(client);SourceFrameClock::Stamp stamp;
        return !stopping_ && !blocked_ && !sourceLost_ && session && session->timestamp(now,stamp) &&
            clients_.change(HalSilenceState::Change::start,client);
    }
    StopResult stop(uint32_t client) {
        std::lock_guard lock(control_);bool ended=false;
        const auto *session=ownerActive(client);
        // A fault can retire/join the session before the host calls StopIO.
        // Preserve device-wide hold accounting even after its buffers are gone.
        if(!session && !sourceLost_)return {};
        if(!clients_.change(HalSilenceState::Change::stop,client,0,&ended))return {};
        if(!ended)return {true,false,true,!blocked_};
        // Close lifetime admission before requesting stop. An already pinned
        // callback may finish; no new one can reach the retiring session.
        gate_.closeAdmission();if(session){session->requestStop();stopping_=true;}
        const bool drained=drain();
        return {true,true,drained,!blocked_};
    }
    bool retireEnded() {
        std::lock_guard lock(control_);const auto *session=gate_.ownerSession();
        if(stopping_ || sourceLost_ || !session || !session->ended())return false;
        gate_.closeAdmission();session->requestStop();
        sourceLost_=true;stopping_=true;return true;
    }
    bool drainStopped(){std::lock_guard lock(control_);return drain();}
    // Final owner teardown only, after external control/callback quiescence.
    // Does not publish property changes: the driver instance is being released.
    bool shutdownAfterQuiescence() {
        std::lock_guard lock(control_);gate_.closeAdmission();
        if(auto *session=gate_.ownerSession()){session->requestStop();stopping_=true;}
        return drain();
    }
    bool cleanupBlocked(){std::lock_guard lock(control_);return blocked_;}
    bool timestamp(uint32_t client,uint64_t now,SourceFrameClock::Stamp &out) noexcept {
        auto pin=active(client);return pin && pin->get().timestamp(now,out);
    }
    bool readInput(uint32_t client,uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,
                   UInt32 frames,std::span<float> out) noexcept {
        auto pin=active(client);
        if(!pin){std::fill(out.begin(),out.end(),0);return false;}
        return pin->get().readInput(now,cycle,frames,out);
    }
    bool writeOutput(uint32_t client,uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,
                     UInt32 frames,std::span<const float> out) noexcept {
        auto pin=active(client);if(!pin)return false;
        if(outputBusy_.test_and_set(std::memory_order_acquire)) {pin->get().requestStop();return false;}
        const bool ok=pin->get().writeOutput(now,cycle,frames,out);
        outputBusy_.clear(std::memory_order_release);return ok;
    }
    // Stop every hold, drain, and externally quiesce ALL callers before destroy.
    // Source loss rejects callbacks but does not erase the host's IO holds.
};
}
