// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativePeerCleanup.hpp"
namespace Pro40Audio {
// Serialized supervisor, never an audio callback. Resources outlive this object
// and own sender/endpoints. Every operation must have a bounded implementation.
// A failed join retains resources: caller must keep them alive until quiescent.
// No destructor cleanup/retry can turn uncertain teardown into claimed success.
template<class Resources> class NativePeerSupervisor {
public:
    explicit NativePeerSupervisor(Resources &resources):resources_(resources){}
    NativePeerSupervisor(const NativePeerSupervisor &)=delete;
    bool arm() noexcept {
        if(armed_||finished_)return false;
        armed_=true;return true;
    }
    int32_t finish() noexcept {
        if(finished_)return result_;
        finished_=true;
        if(!armed_){result_=0;return result_;}
        // Control-port ownership cannot return to this thread before join.
        if(!resources_.quiesceAndJoin())return result_;
        const bool senderClosed=resources_.senderClosed();
        const bool remote=resources_.confirmRemote();
        // Do not short-circuit close when earlier cleanup failed.
        const bool controlClosed=resources_.closeControl();
        result_=peerCleanupStatus(senderClosed&&controlClosed?LocalCleanup::closed:LocalCleanup::incomplete,
                                  remote?RemoteCleanup::confirmed:RemoteCleanup::unconfirmed);
        return result_;
    }
    static int32_t finalize(void *context) noexcept {
        return static_cast<NativePeerSupervisor *>(context)->finish();
    }
private:
    Resources &resources_;
    bool armed_=false,finished_=false;
    int32_t result_=int32_t(PeerCleanupError::localIncomplete);
};
}
