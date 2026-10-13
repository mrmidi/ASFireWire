// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ControllerConnection.hpp"
#include "VerifierSessionOwner.hpp"
#include "VerifierNativeEvents.hpp"
namespace Pro40Lab {
// Call on the serial queue with a bound journal/launch and the SAME operations
// instance that registered. Caller must start the returned owner even on refusal
// (already stopped), then retain/observe it through local finalization. If start
// cannot allocate its timer, caller retains the binding/session, calls
// cancelPreparation and waits for localCleanupComplete before releasing it.
template<class Resources>
struct ControllerSessionBinding {
    std::shared_ptr<VerifierSessionOwner<Resources>> session;
    bool prepared=false;
    std::function<void()> cancelPreparation;
    std::function<bool()> localCleanupComplete;
    std::function<std::optional<Pro40Audio::NativeIdentityResult>()> identityResult;
    std::function<std::optional<Pro40Audio::NativeServiceIdentityOutcome>(uint64_t,bool)> identityOutcome;
};
template<class Resources,class Prepare>
ControllerSessionBinding<Resources> bindControllerSession(dispatch_queue_t queue,std::shared_ptr<Resources> resources,
    LaunchNonce nonce,uint64_t deadline,std::function<uint64_t()> now,
    std::function<VerifierObservation(int32_t)> observe,
    std::function<NSDictionary *()> removal,std::function<void(NSDictionary *)> report,Prepare prepare,
    std::optional<Pro40Audio::NativeIdentityRequest> originalRequest={}){
    dispatch_assert_queue(queue);
    if(originalRequest && (!originalRequest->valid() || originalRequest->requestID!=nonce || !deadline))return {};
    struct Local {std::shared_ptr<ControllerConnection> connection;bool finalized=false,cancelled=false;};
    auto local=std::make_shared<Local>();
    auto cancel=[local]{if(!local->cancelled){local->cancelled=true;if(local->connection)local->connection->cancel();}};
    auto session=VerifierSessionOwner<Resources>::create(queue,resources,now,deadline,
        [local]{return local->finalized;},removal,report,
        [local]{if(local->connection)local->connection->activate();},
        [local]{if(local->connection)local->connection->release();},
        cancel);
    if(!session)return {};
    std::weak_ptr<Local> weakLocal=local;
    auto events=std::make_shared<VerifierNativeEvents<VerifierSessionOwner<Resources>>>(session,observe,now,
        [weakLocal]{if(auto state=weakLocal.lock()){state->finalized=true;state->connection.reset();}},
        [report](const std::string& reason,const VerifierObservation* o){
            NSMutableDictionary *detail=[@{@"mode":@"verifier-failure-diagnostic",
                @"reason":[NSString stringWithUTF8String:reason.c_str()]?:@"unrepresentable",
                @"audio_admitted":@NO} mutableCopy];
            if(o){
                auto text=[](const std::string& value,size_t limit){
                    return [[NSString alloc] initWithBytes:value.data() length:std::min(value.size(),limit)
                        encoding:NSUTF8StringEncoding]?:@"invalid-utf8";
                };
                detail[@"observation"]=@{@"began_ns":@(o->began),@"ended_ns":@(o->ended),
                    @"probed_pid":@(o->probedPID),@"birth_before":@(o->before.has_value()),
                    @"birth_after":@(o->after.has_value()),@"absent_after":@(o->absentAfter),
                    @"job_usable":@(o->job.usable()),@"job_exit":@(o->job.exit),
                    @"job_stdout":text(o->job.out,16384),@"job_stderr":text(o->job.err,1024),
                    @"truncated":@(o->job.out.size()>16384 || o->job.err.size()>1024)};
            }
            report(detail); // One anomaly record, on owner queue, before failure cleanup.
        },originalRequest,deadline);
    if(!session->installExitPoll([events]{events->pollExit();}))return {};
    auto result=prepare(nonce,deadline,events->callbacks());
    local->connection=std::move(result.connection);local->finalized=!result.cleanupPending;
    const bool prepared=bool(local->connection) && result.cleanupPending;
    if(!prepared){
        cancel();
        session->requestStop(); // prevent activation; start records refusal and drains cleanup
    }
    std::weak_ptr<VerifierSessionOwner<Resources>> weakSession=session;
    return {std::move(session),prepared,cancel,[local]{return local->finalized;},[events]{return events->identityResult();},
        [weakSession,events,originalRequest,deadline](uint64_t now,bool stopped)->std::optional<Pro40Audio::NativeServiceIdentityOutcome>{
            if(!originalRequest)return {};
            auto owner=weakSession.lock();
            return verifierIdentityOutcome(*originalRequest,events->identityResult(),deadline,now,stopped,
                owner?owner->completionEvidence():std::nullopt);
        }};
}
}
