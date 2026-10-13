// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "LaunchActionGate.hpp"
#include "VerifierObservedLaunch.hpp"
#include <memory>
namespace Pro40Lab {
// Serial owner retains the journal and IPC resources through cleanup. This
// adapter issues no API by itself; actions must issue immediately inside gates.
class JournaledVerifierLaunch final {
    RegistrationJournal& journal_;
    LaunchActionGate gate_;
    NSDictionary *metadata_;
    NSString *attempt_;
    std::optional<SubmittedVerifierOwnership> ownership_;
    LaunchNonce nonce_;
    uint64_t deadline_;
    std::string target_,program_,plist_;
    VerifierObservedLaunch observed_;
    bool blocked_=false,readySaved_=false,released_=false;
    bool exitSaved_=false,unregisterIssued_=false,resultSaved_=false,removalSaved_=false,complete_=false;
    static std::string utf8(NSString *s){return s.UTF8String;}
    static NSString *output(const VerifierObservation& o){
        return [[NSString alloc] initWithBytes:o.job.out.data() length:o.job.out.size() encoding:NSUTF8StringEncoding];
    }
    NSString *nonceText()const{
        NSMutableString *s=[NSMutableString string];for(auto b:nonce_)[s appendFormat:@"%02x",b];return s;
    }
    bool fail(NSString *detail,uint64_t now){
        if(!blocked_){blocked_=true;(void)gate_.fail(@"observation-invalid",detail,now);}return false;
    }
    JournaledVerifierLaunch(RegistrationJournal& j,NSString *attempt,NSDictionary *m,LaunchNonce n,uint64_t d):
        journal_(j),gate_(j),metadata_([NSJSONSerialization JSONObjectWithData:
            [NSJSONSerialization dataWithJSONObject:m options:0 error:nil] options:0 error:nil]),attempt_([attempt copy]),nonce_(n),deadline_(d),
        target_(utf8([NSString stringWithFormat:@"%@/%@",m[@"domain"],m[@"label"]])),
        program_(utf8([m[@"bundle_path"] stringByAppendingPathComponent:@"Contents/MacOS/Pro40VerifierLab"])),
        plist_(utf8([m[@"bundle_path"] stringByAppendingPathComponent:
            [@"Contents/Library/LaunchAgents" stringByAppendingPathComponent:m[@"plist"]]])),
        observed_(target_,program_,plist_,utf8(m[@"boot"]),n,d){}
public:
    static std::unique_ptr<JournaledVerifierLaunch> bind(RegistrationJournal& j,NSString *attempt,
        NSDictionary *metadata,LaunchNonce nonce,uint64_t deadline){
        if(!j.matchesRegisteredLaunch(attempt,metadata))return {};
        return std::unique_ptr<JournaledVerifierLaunch>(new JournaledVerifierLaunch(j,attempt,metadata,nonce,deadline));
    }
    bool hasSubmittedOwnership()const{return ownership_.has_value();}
    bool bindOwnership(const SubmittedVerifierOwnership& proof){
        if(blocked_ || ownership_ || proof.attempt()!=utf8(attempt_) ||
           !journal_.matchesRegisteredLaunch(attempt_,metadata_) || !observed_.bindOwnership(proof))return false;
        ownership_=proof;return true;
    }
    std::optional<VerifierJobSnapshot> jobSnapshot(const VerifierObservation& o,uint64_t now)const{
        return ownership_?ownership_->snapshot(o.job,target_,program_,plist_,o.began,o.ended,now,deadline_):
            verifierJobSnapshot(o.job,target_,program_,plist_);
    }
    template<class Permit,class Action,class Now>
    bool launch(const VerifierObservation& o,Permit permit,Action action,Now now){
        if(blocked_)return false;
        const auto t=now();
        if(!observed_.prepare(o,true,true,true,t))return fail(@"baseline rejected",t);
        const auto j=jobSnapshot(o,t);NSString *raw=output(o);
        if(!j || !raw)return fail(@"baseline serialization rejected",t);
        NSDictionary *e=@{@"nonce":nonceText(),@"baseline_runs":@(j->runs),@"observed_ns":@(t),
            @"deadline_ns":@(deadline_),@"exact_job":@YES,@"no_running_pid":@YES,@"job_output":raw};
        if(ownership_){
            NSMutableDictionary *bound=[e mutableCopy];
            bound[@"submitted_ownership"]=@{@"uuid":[NSString stringWithUTF8String:ownership_->uuid().c_str()],
                @"target":[NSString stringWithUTF8String:ownership_->target().c_str()],
                @"bundle_path":[NSString stringWithUTF8String:ownership_->bundle().c_str()],
                @"attempt":attempt_,@"observed_ns":@(ownership_->observedNS()),@"deadline_ns":@(deadline_)};
            e=bound;
        }
        bool issued=false;
        const bool saved=gate_.launch(e,permit,[&]{
            if(!observed_.authorizeLaunch(true,now())){(void)fail(@"launch expired after persistence",now());return;}
            issued=true;action();
        },now);
        if(!saved)blocked_=true;
        return saved && issued;
    }
    bool ready(const VerifierObservation& o,const AuthenticatedVerifierPeer& peer,uint64_t now){
        if(blocked_)return false;
        if(!observed_.ready(o,nonce_,peer,now))return fail(@"ready observation rejected",now);
        const auto j=jobSnapshot(o,now);NSString *raw=output(o);
        if(!j || !raw)return fail(@"ready serialization rejected",now);
        const auto &b=peer.birth;
        NSDictionary *e=@{@"nonce":nonceText(),@"pid":@(b.pid),@"launchd_pid":@(*j->pid),
            @"boot":[NSString stringWithUTF8String:b.boot],@"start_seconds":@(b.seconds),@"start_microseconds":@(b.microseconds),
            @"helper_cdhash":metadata_[@"helper_cdhash"],@"authenticated_peer":@YES,@"process_birth_matches":@YES,
            @"runs":@(j->runs),@"observed_ns":@(now),@"job_output":raw};
        readySaved_=journal_.append(@"ready",e);
        if(!readySaved_)blocked_=true;
        return readySaved_;
    }
    template<class Permit,class Action,class Now>
    bool release(Permit permit,Action action,Now now){
        if(blocked_ || !readySaved_ || released_)return false;
        const bool ok=gate_.release(@{@"observed_ns":@(now())},permit,[&]{released_=true;action();},now);
        if(!ok)blocked_=true;return ok;
    }
    bool exited(const VerifierObservation& o,uint64_t now){
        if(blocked_)return false;
        if(!released_ || !observed_.exited(o,now))return fail(@"exit observation rejected",now);
        auto j=jobSnapshot(o,now);NSString *raw=output(o);
        if(!j || !raw)return fail(@"exit serialization rejected",now);
        const bool ok=journal_.append(@"exit",@{@"runs":@(j->runs),@"observed_ns":@(now),
            @"code":@(*j->lastExitCode),@"exit_code_known":@YES,@"signalled":@NO,@"exact_job":@YES,
            @"not_running":@YES,@"original_instance_gone":@YES,@"job_output":raw});
        if(!ok)blocked_=true;exitSaved_=ok;return ok;
    }
    template<class Permit,class Action,class Now>
    bool unregister(Permit permit,Action action,Now now){
        if(blocked_ || !exitSaved_ || unregisterIssued_)return false;
        const bool ok=gate_.unregister(permit,[&]{unregisterIssued_=true;action();},now);
        if(!ok)blocked_=true;return ok;
    }
    // Call only from the retained asynchronous service completion. Recording its
    // result does not prove removal, local IPC cleanup, or successful helper exit.
    bool unregisterReturned(NSDictionary *result){
        if(blocked_ || !unregisterIssued_ || resultSaved_)return false;
        resultSaved_=journal_.append(@"unregister_result",result);
        if(!resultSaved_ || [result[@"ownership_conflict"] boolValue])blocked_=true;
        return !blocked_;
    }
    bool removed(NSDictionary *evidence,uint64_t now){
        if(blocked_ || !resultSaved_ || removalSaved_)return false;
        if(!journal_.append(@"removal",evidence)){blocked_=true;return false;}
        if(!observed_.removed([evidence[@"not_registered"] boolValue],
            [evidence[@"job_absent"] boolValue],![evidence[@"unexpected_helper"] boolValue],now))
            return fail(@"independent removal rejected",now);
        removalSaved_=true;return true;
    }
    // False local cleanup means "keep waiting", never fake completion. The
    // resource owner must supply its finalizer/handle accounting, not peer claims.
    bool complete(bool localIPCFinalized,uint64_t now){
        if(blocked_ || complete_ || !removalSaved_ || !localIPCFinalized)return false;
        if(!observed_.complete(true,now))return fail(@"completion deadline rejected",now);
        complete_=journal_.append(@"complete",@{});
        if(!complete_)blocked_=true;return complete_;
    }
    bool reconciled()const{return complete_ && !blocked_;}
    bool cleanExitAndRemoval()const{return reconciled() && observed_.cleanExitAndRemoval();}
    void abandon(NSString *category,NSString *detail,uint64_t now){
        if(blocked_ || complete_)return;
        blocked_=true;(void)gate_.fail(category,detail,now);
    }
};
}
