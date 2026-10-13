// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeVerifierConnection.hpp"
#include "VerifierObservedLaunch.hpp"
namespace Pro40Lab {
// Owner-queue bridge for actual NativeVerifierConnection callbacks. The session
// retains this bridge/connection through finalization; callbacks use weak session
// ownership so they cannot form a session/connection ownership cycle.
template<class Session>
class VerifierNativeEvents final : public std::enable_shared_from_this<VerifierNativeEvents<Session>> {
    std::weak_ptr<Session> session_;
    std::function<VerifierObservation(int32_t)> observe_;
    std::function<uint64_t()> now_;
    std::function<void()> finalized_;
    std::optional<NativeProcessBirth> peer_;
    uint64_t runs_=0,lastObservation_=0;
    bool localFinalized_=false,diagnosed_=false;
    std::function<void(const std::string&,const VerifierObservation*)> diagnose_;
    const std::optional<Pro40Audio::NativeIdentityRequest> expected_;
    const uint64_t identityDeadline_;
    std::optional<AuthenticatedVerifierPeer> pendingReady_;
    std::optional<Pro40Audio::NativeIdentityResult> identityResult_;
    bool identityBlocked_=false;
    void diagnose(const std::string& reason,const VerifierObservation* observation=nullptr){
        identityBlocked_=true;
        if(diagnosed_)return;
        diagnosed_=true;
        if(diagnose_)diagnose_(reason,observation);
    }
    void progress(ExchangeKind kind,const AuthenticatedVerifierPeer& peer){
        auto session=session_.lock();if(!session)return;
        if(expected_){
            if(identityBlocked_ || localFinalized_){session->failed();return;}
            if(kind==ExchangeKind::ready){
                if(pendingReady_ || peer_ || identityResult_ || !session->acceptsReady() ||
                   !expected_->valid() || !identityDeadline_ || now_()>=identityDeadline_){
                    identityBlocked_=true;diagnose("identity-ready-out-of-order");session->failed();return;
                }
                // Do not call session.ready yet: that method journals release.
                pendingReady_=peer;return;
            }
            if(!identityResult_){identityBlocked_=true;diagnose("progress-before-identity-result");session->failed();return;}
        }
        deliverProgress(kind,peer);
    }
    void deliverProgress(ExchangeKind kind,const AuthenticatedVerifierPeer& peer){
        auto session=session_.lock();if(!session)return;
        if(kind==ExchangeKind::ready){
            if(peer_ || !session->acceptsReady()){diagnose("ready-out-of-order");session->failed();return;}
            auto o=observe_(peer.birth.pid);
            auto job=session->jobSnapshot(o,now_());
            if(!job){diagnose("ready-job-observation-rejected",&o);session->failed();return;}
            // Session performs the authoritative readiness/birth/freshness gate.
            if(!session->ready(o,peer)){diagnose("ready-admission-rejected",&o);return;}
            peer_=peer.birth;runs_=job->runs;lastObservation_=o.ended;
        }else if(kind==ExchangeKind::released)session->released(peer);
        else {diagnose("unexpected-progress-kind");session->failed();}
    }
    void identity(const Pro40Audio::NativeIdentityResult& result,const AuthenticatedVerifierPeer& peer){
        auto session=session_.lock();if(!session)return;
        const auto time=now_();
        if(!expected_ || identityBlocked_ || localFinalized_ || !pendingReady_ || identityResult_ ||
           !identityDeadline_ || time>=identityDeadline_ || !session->acceptsReady() ||
           result.version!=1 || !(result.request==*expected_) ||
           !sameVerifierBirth(pendingReady_->birth,peer.birth) || !pendingReady_->observedNS ||
           peer.observedNS<pendingReady_->observedNS || peer.observedNS>time){
            identityBlocked_=true;diagnose("identity-result-out-of-order-or-mismatched");session->failed();return;
        }
        identityResult_=result;pendingReady_.reset();
        // Fresh independent job observation is taken now, after verification.
        // A nonzero result still needs orderly release/exit/removal, never admission.
        deliverProgress(ExchangeKind::ready,peer);
    }
public:
    VerifierNativeEvents(std::weak_ptr<Session> session,std::function<VerifierObservation(int32_t)> observe,
        std::function<uint64_t()> now,std::function<void()> finalized,
        std::function<void(const std::string&,const VerifierObservation*)> diagnose={},
        std::optional<Pro40Audio::NativeIdentityRequest> expected={},uint64_t identityDeadline=0):session_(std::move(session)),observe_(std::move(observe)),
        now_(std::move(now)),finalized_(std::move(finalized)),diagnose_(std::move(diagnose)),expected_(std::move(expected)),identityDeadline_(identityDeadline){}
    // Protocol evidence only. Caller must separately establish clean exit/removal.
    std::optional<Pro40Audio::NativeIdentityResult> identityResult()const{
        return identityBlocked_?std::nullopt:identityResult_;
    }
    NativeVerifierConnection::Callbacks callbacks(){
        auto retained=this->shared_from_this();
        return {[retained](ExchangeKind kind,const AuthenticatedVerifierPeer& peer){retained->progress(kind,peer);},
            [retained](std::string reason){retained->identityBlocked_=true;if(auto session=retained->session_.lock()){retained->diagnose("connection:"+reason);session->failed();}},
            [retained]{if(!retained->localFinalized_){retained->localFinalized_=true;retained->finalized_();}},
            [retained](const Pro40Audio::NativeIdentityResult& result,const AuthenticatedVerifierPeer& peer){retained->identity(result,peer);}};
    }
    // Call at bounded cadence on the owner queue after ACK, not from an XPC
    // finalizer. A finalizer is local cleanup evidence, never a remote exit.
    void pollExit(){
        auto session=session_.lock();if(!session || !session->awaitsExit())return;
        if(!peer_){diagnose("exit-without-ready-peer");session->failed();return;}
        auto o=observe_(peer_->pid);const auto now=now_();
        auto job=session->jobSnapshot(o,now);
        if(!job || o.probedPID!=peer_->pid || !o.began || o.began<lastObservation_ ||
           o.ended<o.began || now<o.ended || now-o.began>=5000000000ULL || job->runs!=runs_){
            diagnose("exit-observation-rejected",&o);session->failed();return;
        }
        lastObservation_=o.ended;
        if(job->running){
            if(!job->pid || *job->pid!=peer_->pid || o.absentAfter || !o.before || !o.after ||
               !sameVerifierBirth(*peer_,*o.before) || !sameVerifierBirth(*peer_,*o.after)){diagnose("exit-live-peer-changed",&o);session->failed();}
            return; // same launched process is still finishing; do not claim exit
        }
        session->exited(o); // authoritative independent-exit/journal gate
    }
};
}
