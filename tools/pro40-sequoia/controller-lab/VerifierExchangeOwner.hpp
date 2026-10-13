// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "JournaledVerifierLaunch.hpp"
#include <functional>
namespace Pro40Lab {
enum class VerifierExchangeState { initial, awaitingReady, awaitingRelease, awaitingExit, cleanup, blocked };
// Serial-queue coordinator. Caller retains journal/launch and local IPC resources
// until cleanup retirement. Observations must be independently collected; this
// object does not infer exit from an acknowledgement or disconnect.
class VerifierExchangeOwner final {
    JournaledVerifierLaunch& launch_;
    std::function<ActionPermission()> permit_;
    std::function<uint64_t()> now_;
    std::function<void()> activate_,release_,cancel_,cleanup_;
    std::optional<NativeProcessBirth> peer_;
    uint64_t lastPeerTime_=0,releaseTime_=0;
    VerifierExchangeState state_=VerifierExchangeState::initial;
    bool cancelled_=false;
    void block(NSString *detail,NSString *category=@"transport-error"){
        if(state_==VerifierExchangeState::blocked || state_==VerifierExchangeState::cleanup)return;
        state_=VerifierExchangeState::blocked;
        launch_.abandon(category,detail,now_());
        cancelOnce();
    }
    void cancelOnce(){if(!cancelled_){cancelled_=true;cancel_();}}
    bool permitted(){
        const auto p=permit_();if(p==ActionPermission::allowed)return true;
        block(@"exchange permission denied",p==ActionPermission::cancelled?@"cancelled":
            p==ActionPermission::expired?@"expired":@"observation-invalid");return false;
    }
public:
    VerifierExchangeOwner(JournaledVerifierLaunch& launch,std::function<ActionPermission()> permit,
        std::function<uint64_t()> now,std::function<void()> activate,std::function<void()> release,
        std::function<void()> cancel,std::function<void()> cleanup):launch_(launch),permit_(std::move(permit)),
        now_(std::move(now)),activate_(std::move(activate)),release_(std::move(release)),
        cancel_(std::move(cancel)),cleanup_(std::move(cleanup)){}
    bool start(const VerifierObservation& baseline){
        if(state_!=VerifierExchangeState::initial)return false;
        if(!launch_.launch(baseline,permit_,[&]{state_=VerifierExchangeState::awaitingReady;activate_();},now_)){
            block(@"launch gate rejected");return false;
        }
        return state_!=VerifierExchangeState::blocked;
    }
    bool ready(const VerifierObservation& running,const AuthenticatedVerifierPeer& peer){
        if(state_!=VerifierExchangeState::awaitingReady){block(@"unexpected ready");return false;}
        if(!permitted() || !launch_.ready(running,peer,now_())){
            block(@"ready rejected");return false;
        }
        peer_=peer.birth;lastPeerTime_=peer.observedNS;
        if(!launch_.release(permit_,[&]{releaseTime_=now_();state_=VerifierExchangeState::awaitingRelease;release_();},now_)){
            block(@"release gate rejected");return false;
        }
        return state_!=VerifierExchangeState::blocked;
    }
    bool released(const AuthenticatedVerifierPeer& peer){
        if(state_!=VerifierExchangeState::awaitingRelease || !peer_ ||
           !sameVerifierBirth(*peer_,peer.birth) || peer.observedNS<lastPeerTime_ || peer.observedNS<releaseTime_ ||
           peer.observedNS>now_() || !permitted()){
            block(@"release acknowledgement rejected");return false;
        }
        lastPeerTime_=peer.observedNS;state_=VerifierExchangeState::awaitingExit;
        // ACK was received. Closing now permits the helper to finish locally.
        cancelOnce();return true;
    }
    bool exited(const VerifierObservation& terminal){
        if(state_!=VerifierExchangeState::awaitingExit){block(@"exit before acknowledgement");return false;}
        if(terminal.began<lastPeerTime_ || !permitted() || !launch_.exited(terminal,now_())){
            block(@"independent exit rejected");return false;
        }
        state_=VerifierExchangeState::cleanup;cleanup_();return true;
    }
    void failed(){block(@"connection failed before independent exit");}
    void poll(){
        if(state_!=VerifierExchangeState::cleanup && state_!=VerifierExchangeState::blocked)(void)permitted();
    }
    VerifierExchangeState state()const{return state_;}
};
}
