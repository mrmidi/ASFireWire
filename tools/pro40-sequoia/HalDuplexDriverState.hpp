// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalDuplexState.hpp"
#include "NativeDuplexPacket.hpp"
#include <functional>
#include <mach/mach_time.h>
#include <os/log.h>
namespace Pro40Audio {
// Process-local connection boundary. A concrete adapter owns its buffers and
// authenticated worker connection. All native/IPC work and cleanup remain off
// the realtime callbacks. Its destructor must join outstanding users as well.
class HalDuplexSession {
public:
    const NativeDuplexDescriptor descriptor;
    explicit HalDuplexSession(const NativeDuplexDescriptor &d):descriptor(d){}
    virtual ~HalDuplexSession()=default;
    virtual bool ready(uint64_t now) const noexcept=0;
    // Published sessions only. Monotonic terminal signal, unlike ready(),
    // whose initial input/clock admission window naturally expires during IO.
    virtual bool ended() const noexcept=0;
    virtual void requestStop() const noexcept=0;
    virtual bool stopAndJoin()=0;
    virtual bool timestamp(uint64_t now,SourceFrameClock::Stamp &) const noexcept=0;
    virtual bool readInput(uint64_t now,const AudioServerPlugInIOCycleInfo &,UInt32,std::span<float>) const noexcept=0;
    virtual bool writeOutput(uint64_t now,const AudioServerPlugInIOCycleInfo &,UInt32,std::span<const float>) const noexcept=0;
};
// Factory is trusted local configuration, not an incoming IPC capability. It
// must perform bounded startup/handshake and return only a fully owned activated
// session (or a failure carrying cleanup status). No factory is configured in an ordinary factory instance:
// the prototype cannot start fictitious device IO by itself.
struct HalDuplexPreparation {std::unique_ptr<HalDuplexSession> session;bool cleanupOK=true;};
using HalDuplexPrepare=std::function<HalDuplexPreparation(uint64_t seed,uint32_t numer,uint32_t denom)>;
class HalDuplexDriverState {
    HalDuplexState<HalDuplexSession> state_;
    std::mutex control_;
    HalDuplexPrepare prepare_;
    bool configured_=false,attempted_=false,blocked_=false,cleanupReported_=false,sourceLost_=false;
    void reportCleanupFailure() {
        if(!cleanupReported_) {
            cleanupReported_=true;
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab duplex cleanup failed; further startup is blocked");
        }
    }
public:
    using Change=HalSilenceState::Change;
    HalDuplexDriverState(uint32_t numer,uint32_t denom):state_(numer,denom){}
    ~HalDuplexDriverState(){if(!state_.shutdownAfterQuiescence())reportCleanupFailure();}
    bool configure(HalDuplexPrepare prepare) {
        std::lock_guard lock(control_);
        if(configured_ || attempted_ || !prepare)return false;
        prepare_=std::move(prepare);configured_=true;return true;
    }
    bool read(HalSilenceModel::Snapshot &s) const noexcept {return state_.snapshot(s);}
    bool change(Change action,uint32_t client,uint64_t now,bool *changed) {
        std::lock_guard lock(control_);if(changed)*changed=false;
        if(action==Change::add)return state_.add(client);
        if(action==Change::remove)return state_.remove(client);
        if(action==Change::stop) {
            const auto result=state_.stop(client);if(changed)*changed=result.runningChanged;
            if(!result.cleanupOK)reportCleanupFailure();
            // Delayed reclamation does not undo an accepted StopIO. Subsequent
            // StartIO must drain first; final Release also drains after quiescence.
            return result.accepted;
        }
        HalSilenceModel::Snapshot s;if(!state_.snapshot(s) || !s.known(client))return false;
        if(sourceLost_)return false; // Recovery requires a fresh driver owner.
        if(s.running())return state_.retainIO(client,now);
        attempted_=true;
        if(blocked_ || !prepare_ || s.seed==UINT64_MAX)return false;
        if(!state_.drainStopped()){if(state_.cleanupBlocked())reportCleanupFailure();return false;}
        auto result=prepare_(s.seed+1,s.numer,s.denom);
        if(!result.cleanupOK){blocked_=true;reportCleanupFailure();}
        auto prepared=std::move(result.session);
        if(!prepared)return false;
        if(blocked_ || prepared->descriptor.inputChannels!=8 || prepared->descriptor.outputChannels!=6 ||
           !state_.start(client,prepared,mach_absolute_time())) {
            if(!prepared->stopAndJoin()){blocked_=true;reportCleanupFailure();}return false;
        }
        if(changed)*changed=true;return true;
    }
    // Control-thread source loss is independent of the host's outstanding IO
    // holds. Close admission first, then call the host with NO owner lock held.
    bool retireEnded() {
        std::lock_guard lock(control_);
        if(sourceLost_ || !state_.retireEnded())return false;
        sourceLost_=true;return true;
    }
    enum class LossDrain {pending,clean,failed};
    LossDrain drainSourceLoss() {
        std::lock_guard lock(control_);
        if(!sourceLost_)return LossDrain::pending;
        if(state_.drainStopped())return LossDrain::clean;
        if(!state_.cleanupBlocked())return LossDrain::pending;
        reportCleanupFailure();return LossDrain::failed;
    }
    bool timestamp(uint32_t client,uint64_t now,HalSilenceModel::Stamp &out) noexcept {
        SourceFrameClock::Stamp stamp;if(!state_.timestamp(client,now,stamp))return false;
        out={stamp.sample,stamp.host,stamp.seed};return true;
    }
    struct ReadResult {bool valid=false;bool ok() const noexcept {return valid;}};
    ReadResult readInput(uint32_t client,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) noexcept {
        return {state_.readInput(client,mach_absolute_time(),cycle,frames,out)};
    }
    bool writeOutput(uint32_t client,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<const float> out) noexcept {
        return state_.writeOutput(client,mach_absolute_time(),cycle,frames,out);
    }
};
}
