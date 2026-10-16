// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalSilenceState.hpp"
#include "AudioSyntheticSession.hpp"
namespace Pro40Audio {
struct HalSyntheticChange {
    bool accepted=false,runningChanged=false;
    SyntheticControlResult session=SyntheticControlResult::ok;
};
// Composition for local HAL-adapter tests. Reuses the established registered-
// client/device-wide hold model. Host notifications belong AFTER method return,
// never under control locks. No live driver selects this state yet.
class HalSyntheticState {
    std::mutex control_;
    HalSilenceState state_;
    AudioSyntheticSessionControl sessions_;
    bool blocked_=false;
    using Lease=AudioSessionGate<AudioSyntheticSession>::Lease;
    std::optional<Lease> active(uint32_t client,HalSilenceModel::Snapshot &snapshot) noexcept {
        auto pin=sessions_.tryAcquire();if(!pin)return {};
        if(!state_.read(snapshot) || !snapshot.active(client))return {};
        const auto &clock=pin->get().clock();
        if(clock.seed!=snapshot.seed || clock.origin!=snapshot.anchor ||
           clock.numer!=snapshot.numer || clock.denom!=snapshot.denom)return {};
        return pin;
    }
public:
    HalSyntheticState(uint32_t numer,uint32_t denom):state_(numer,denom) {}
    bool snapshot(HalSilenceModel::Snapshot &out) const noexcept {return state_.read(out);}
    HalSyntheticChange change(HalSilenceState::Change action,uint32_t client,
                              mach_port_t service=MACH_PORT_NULL,const AudioSyntheticStart *proposal=nullptr) {
        std::lock_guard lock(control_);
        HalSyntheticChange out;HalSilenceModel::Snapshot before;
        if(!state_.read(before))return out;
        if(action==HalSilenceState::Change::start && !before.running()) {
            if(blocked_){out.session=SyntheticControlResult::cleanupFailed;return out;}
            if(!before.known(client) || !proposal || before.seed==UINT64_MAX ||
               proposal->seed!=before.seed+1 || proposal->numer!=before.numer || proposal->denom!=before.denom) {
                out.session=SyntheticControlResult::invalidClock;return out;
            }
            out.session=sessions_.start(service,*proposal);
            if(out.session!=SyntheticControlResult::ok)return out;
            // Control-only wait. Avoid publishing a future zero timestamp that
            // the existing clock model must reject. Synthetic origin is bounded
            // by admission to <=5s; scheduler delay is still possible.
            while(mach_absolute_time()<proposal->origin)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            bool finished=true;
            {auto pin=sessions_.tryAcquire();if(pin)finished=pin->get().sourceFinished();}
            if(finished) {
                const auto cleanup=sessions_.stop();blocked_=true;
                out.session=cleanup==SyntheticControlResult::ok?SyntheticControlResult::preparationFailed:cleanup;
                return out; // Published session seed was consumed; don't reuse it.
            }
            out.accepted=state_.change(action,client,proposal->origin,&out.runningChanged);
            if(!out.accepted){out.session=sessions_.stop();blocked_=true;}
            return out;
        }
        out.accepted=state_.change(action,client,0,&out.runningChanged);
        if(out.accepted && action==HalSilenceState::Change::stop && out.runningChanged)
            out.session=sessions_.stop(); // May be busy; never roll back final IO hold.
        return out;
    }
    // Schedule from control path if final stop reports busy. No callback waits.
    SyntheticControlResult drainStopped() {
        std::lock_guard lock(control_);HalSilenceModel::Snapshot s;
        if(!state_.read(s) || s.running())return SyntheticControlResult::busy;
        return sessions_.stop();
    }
    bool timestamp(uint32_t client,uint64_t now,HalSilenceModel::Stamp &out) noexcept {
        HalSilenceModel::Snapshot s;auto pin=active(client,s);if(!pin)return false;
        return HalSilenceModel::timestamp(s,now,out);
    }
    CallbackSessionResult readInput(uint32_t client,const AudioServerPlugInIOCycleInfo &cycle,
                                   UInt32 frames,std::span<float> out) noexcept {
        HalSilenceModel::Snapshot s;auto pin=active(client,s);
        if(!pin){std::fill(out.begin(),out.end(),0);return {};}
        return pin->get().read(pin->get().clock(),cycle,frames,out);
    }
    // Owner must stop IO, drain sessions, and quiesce all callers before teardown.
};
}
