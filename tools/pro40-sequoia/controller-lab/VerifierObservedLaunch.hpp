#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SubmittedVerifierIdentity.hpp"
#include "VerifierLaunchPolicy.hpp"
#include "AuthenticatedVerifierPeer.hpp"
#include "../NativeProcessBirth.hpp"
#include <strings.h>
namespace Pro40Lab {
struct VerifierObservation {
    uint64_t began=0,ended=0;
    int32_t probedPID=0;
    JobProcessResult job;
    std::optional<NativeProcessBirth> before,after;
    bool absentAfter=false; // only explicit proc_pidinfo ESRCH, never generic failure
};
inline uint64_t observationNS(){return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();}
// Read-only adapter. Target and PID must come from the retained owner identity,
// never from helper message fields. No registration, signal or audio operation.
inline VerifierObservation collectVerifierObservation(const std::string& target,int32_t pid=0) {
    VerifierObservation o;o.began=observationNS();o.probedPID=pid;
    if(pid>0)o.before=NativeProcessBirth::read(pid);
    o.job=observeProcess({"/bin/launchctl","print",target});
    if(pid>0){
        o.after=NativeProcessBirth::read(pid);
        if(!o.after){
            proc_bsdinfo info{};errno=0;
            const int n=proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&info,sizeof(info));
            o.absentAfter=n==0 && errno==ESRCH;
        }
    }
    o.ended=observationNS();return o;
}
class VerifierObservedLaunch final {
    std::string target_,program_,plist_,boot_;
    VerifierLaunchPolicy policy_;
    std::optional<SubmittedVerifierOwnership> ownership_;
    uint64_t deadline_,last_=0;
    std::optional<NativeProcessBirth> instance_;
    static bool valid(const NativeProcessBirth& b){
        const auto *end=static_cast<const char *>(std::memchr(b.boot,0,sizeof(b.boot)));
        if(!end || end-b.boot!=36 || b.pid<=0 || !b.seconds || b.microseconds>=1000000)return false;
        for(size_t i=0;i<36;++i){
            const char c=b.boot[i];
            if(i==8||i==13||i==18||i==23){if(c!='-')return false;}
            else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;
        }
        return true;
    }
    static bool same(const NativeProcessBirth& a,const NativeProcessBirth& b){
        return valid(a)&&valid(b)&&a.pid==b.pid&&!strcasecmp(a.boot,b.boot)&&
            a.seconds==b.seconds&&a.microseconds==b.microseconds;
    }
    bool block(){policy_.abandon();return false;}
    std::optional<VerifierJobSnapshot> snapshot(const VerifierObservation& o,uint64_t now){
        if(!o.began || o.began<last_ || o.ended<o.began || now<o.ended || now>=deadline_ ||
            now-o.began>=5000000000ULL){block();return {};}
        auto job=ownership_ ? ownership_->snapshot(o.job,target_,program_,plist_,o.began,o.ended,now,deadline_) :
            verifierJobSnapshot(o.job,target_,program_,plist_);
        if(!job){block();return {};}
        last_=now;return job;
    }
public:
    VerifierObservedLaunch(std::string target,std::string program,std::string plist,
        std::string controllerBoot,LaunchNonce nonce,uint64_t deadline,
        std::optional<SubmittedVerifierOwnership> ownership={}):
        target_(std::move(target)),program_(std::move(program)),plist_(std::move(plist)),
        boot_(std::move(controllerBoot)),policy_(nonce,deadline),ownership_(std::move(ownership)),deadline_(deadline){}
    bool bindOwnership(const SubmittedVerifierOwnership& proof){
        if(ownership_ || policy_.phase()!=LaunchPhase::initial || proof.target()!=target_ ||
           proof.deadlineNS()!=deadline_ || program_!=proof.bundle()+"/Contents/MacOS/Pro40VerifierLab" ||
           plist_!=proof.bundle()+PRO40_LAB_PLIST_SUFFIX)return false;
        ownership_=proof;return true;
    }
    LaunchPhase phase()const{return policy_.phase();}
    const std::optional<NativeProcessBirth>& instance()const{return instance_;}
    bool prepare(const VerifierObservation& o,bool packageVerified,bool exclusiveJournal,
                 bool registered,uint64_t now){
        auto j=snapshot(o,now);
        if(!j || o.probedPID || j->running || j->pid || j->runs>=INT64_MAX)return block();
        return policy_.prepare({packageVerified,exclusiveJournal,registered,true,true,j->runs},now);
    }
    bool authorizeLaunch(bool durable,uint64_t now){
        if(now<last_)return block();last_=now;return policy_.authorizeLaunch(durable,now);
    }
    // peerBirth must be retained trusted evidence from the authenticated peer
    // observation, not a birth value supplied in its readiness message.
    bool ready(const VerifierObservation& o,LaunchNonce echoedNonce,bool authenticated,
               int32_t kernelPeerPID,const NativeProcessBirth& peerBirth,uint64_t now){
        auto j=snapshot(o,now);
        if(!j || !j->running || !j->pid || kernelPeerPID<=0 || o.probedPID!=kernelPeerPID ||
            !o.before || !o.after || o.absentAfter || !same(*o.before,*o.after) || !same(*o.after,peerBirth) ||
            o.after->pid!=kernelPeerPID || boot_.size()!=36 || strcasecmp(o.after->boot,boot_.c_str()))return block();
        if(!policy_.observeReady({echoedNonce,authenticated,true,true,*j->pid,kernelPeerPID,j->runs},now))return false;
        instance_=o.after;return true;
    }
    bool exited(const VerifierObservation& o,uint64_t now){
        auto j=snapshot(o,now);
        if(!j || !instance_ || j->running || j->pid || !j->lastExitCode || o.probedPID!=instance_->pid)return block();
        bool gone=false;
        if(o.after){
            if(o.absentAfter || !valid(*o.after) || o.after->pid!=instance_->pid ||
                strcasecmp(o.after->boot,instance_->boot))return block();
            gone=!same(*o.after,*instance_);
        }else gone=o.absentAfter;
        return policy_.observeExit({true,true,true,gone,true,false,j->runs,
            static_cast<int32_t>(*j->lastExitCode)},now);
    }
    // Direct bridge from NativeVerifierConnection's authenticated progress event.
    // Peer validation must happen after launch permission and before job sampling.
    bool ready(const VerifierObservation& o,LaunchNonce echoedNonce,
               const AuthenticatedVerifierPeer& peer,uint64_t now){
        if(!peer.observedNS || peer.observedNS<last_ || peer.observedNS>o.began)return block();
        return ready(o,echoedNonce,true,peer.birth.pid,peer.birth,now);
    }
    // Remaining cleanup/completion facts still come from their independent owner.
    bool removed(bool notRegistered,bool jobAbsent,bool noUnexpectedHelper,uint64_t now){
        return policy_.observeRemoval(notRegistered,jobAbsent,noUnexpectedHelper,now);
    }
    bool complete(bool durable,uint64_t now){return policy_.recordCompletion(durable,now);}
    bool cleanExitAndRemoval()const{return policy_.confirmedCleanExitAndRemoval();}
};
}
