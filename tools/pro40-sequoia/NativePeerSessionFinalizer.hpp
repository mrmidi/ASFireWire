// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativePeerSession.hpp"
#include "NativeSessionRunner.hpp"
namespace Pro40Audio {
// Bridge to runner's synchronous post-native-close hook. Arm only within native
// entry, BEFORE creating peer resources; never provision before runner entry.
// Resources supplies bounded cancel(), confirmRemote(), closeControl(). cancel
// must release cooperative waits but not close endpoints under a running worker.
// Session/resources outlive hook return AND any unsuccessful join.
template<class Resources> class NativePeerSessionFinalizer {
public:
    NativePeerSessionFinalizer(NativePeerSession &session,Resources &resources,
                              NativePeerSession::Wall::duration joinBudget)
        :session_(session),resources_(resources),budget_(joinBudget){}
    NativePeerSessionFinalizer(const NativePeerSessionFinalizer &)=delete;
    bool arm() noexcept {
        if(armed_||finished_||budget_<=NativePeerSession::Wall::duration::zero())return false;
        armed_=true;return true;
    }
    Pro40::PeerCleanupFinalizer hook() noexcept {return {finalize,this};}
    int32_t finish() noexcept {
        if(finished_)return result_;
        finished_=true;
        if(!armed_)return result_=0; // Native open failed without provisioning a peer.
        session_.cancel();resources_.cancel();
        return result_=session_.finish(NativePeerSession::Wall::now()+budget_,
            [&]{return resources_.confirmRemote();},[&]{return resources_.closeControl();});
    }
    static int32_t finalize(void *context) noexcept {
        return static_cast<NativePeerSessionFinalizer *>(context)->finish();
    }
private:
    NativePeerSession &session_;Resources &resources_;NativePeerSession::Wall::duration budget_;
    bool armed_=false,finished_=false;int32_t result_=int32_t(PeerCleanupError::localIncomplete);
};
}
