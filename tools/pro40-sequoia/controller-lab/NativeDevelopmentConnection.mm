#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
// Isolate explicit XPC/dispatch ownership from the app's Objective-C ARC types.
#define OS_OBJECT_USE_OBJC 0
#include "NativeDevelopmentConnection.hpp"
#include "NativeDevelopmentIdentity.hpp"
#include "DevelopmentOriginalPeerPolicy.hpp"
#include "DevelopmentConnectionCommand.hpp"
#include "ControllerConnection.hpp"
#include <atomic>
namespace Pro40Lab {
namespace {
class ControllerHandle final : public ControllerConnection {
    std::shared_ptr<NativeVerifierConnection> connection_;
public:
    explicit ControllerHandle(std::shared_ptr<NativeVerifierConnection> c):connection_(std::move(c)){}
    void activate()override{connection_->activate();}
    void release()override{connection_->release(true);}
    void cancel()override{connection_->cancel();}
};
}
ControllerPreparation prepareControllerConnection(void *serialQueue,LaunchNonce nonce,uint64_t deadline,
    NativeVerifierConnection::Callbacks callbacks,std::optional<Pro40Audio::NativeIdentityRequest> originalRequest){
    if(!serialQueue)return {};
    auto result=prepareDevelopmentController(static_cast<dispatch_queue_t>(serialQueue),nonce,deadline,std::move(callbacks),std::move(originalRequest));
    return {result.connection?std::make_shared<ControllerHandle>(std::move(result.connection)):nullptr,result.cleanupPending};
}
DevelopmentControllerPreparation prepareDevelopmentController(dispatch_queue_t queue,
    LaunchNonce nonce,uint64_t deadline,NativeVerifierConnection::Callbacks callbacks,
    std::optional<Pro40Audio::NativeIdentityRequest> originalRequest){
    dispatch_assert_queue(queue);
    if(deadline<=verifierMonotonicNS() || !decodeExchange(encodeExchange({ExchangeKind::challenge,nonce})))return {};
    NSString *requirement=developmentPeerRequirement(DevelopmentRole::controller);
    if(!requirement || deadline<=verifierMonotonicNS())return {};
    auto identity=developmentOriginalIdentity(false,originalRequest);
    if((developmentOriginalPeerEnabled && !identity) || (!developmentOriginalPeerEnabled && originalRequest))return {};
    // SDK xpc/connection.h:170-204: creation does not query the namespace.
    // Activation belongs exclusively inside the journal's durable launch gate.
    auto raw=xpc_connection_create_mach_service(PRO40_LAB_SERVICE_ID,queue,0);
    if(!raw)return {};
    auto connection=NativeVerifierConnection::create(raw,queue,false,nonce,deadline,
        requirement.UTF8String,callbacks,std::move(identity));
    if(!connection){
        // A factory refusal does not discharge local resource ownership.
        struct Disposal {dispatch_queue_t queue;std::function<void()> finalized;};
        dispatch_retain(queue);
        xpc_connection_set_context(raw,new Disposal{queue,std::move(callbacks.finalized)});
        xpc_connection_set_finalizer_f(raw,[](void *context){
            auto d=std::shared_ptr<Disposal>(static_cast<Disposal *>(context));
            dispatch_async(d->queue,^{if(d->finalized)d->finalized();dispatch_release(d->queue);});
        });
        xpc_connection_set_event_handler(raw,^(xpc_object_t){});
        xpc_connection_cancel(raw);xpc_connection_activate(raw);
    }
    xpc_release(raw);return {std::move(connection),true};
}
std::shared_ptr<NativeVerifierConnection> prepareDevelopmentConnection(
    xpc_connection_t c,dispatch_queue_t queue,bool helper,LaunchNonce nonce,
    uint64_t deadline,NativeVerifierConnection::Callbacks callbacks,
    std::optional<Pro40Audio::NativeIdentityRequest> originalRequest) {
    dispatch_assert_queue(queue);
    NSString *requirement=developmentPeerRequirement(helper?DevelopmentRole::helper:DevelopmentRole::controller);
    if(!requirement)return {};
    auto identity=developmentOriginalIdentity(helper,originalRequest);
    if((developmentOriginalPeerEnabled && !identity) || (!developmentOriginalPeerEnabled && originalRequest))return {};
    return NativeVerifierConnection::create(c,queue,helper,nonce,deadline,
        requirement.UTF8String,std::move(callbacks),std::move(identity));
}
int developmentConnectionCommand(bool helper) {
    // Only an anonymous endpoint. No named service lookup, activation for work,
    // messages, registration, journal or hardware operations.
    auto queue=dispatch_queue_create("pro40.development-preparation",DISPATCH_QUEUE_SERIAL);
    auto finished=dispatch_semaphore_create(0);
    auto finalized=std::make_shared<std::atomic<unsigned>>(0);
    auto accepted=std::make_shared<std::atomic<unsigned>>(0);
    auto listener=xpc_connection_create(nullptr,queue);
    xpc_connection_set_context(listener,new std::shared_ptr<std::atomic<unsigned>>(finalized));
    xpc_connection_set_finalizer_f(listener,[](void *context){
        auto *count=static_cast<std::shared_ptr<std::atomic<unsigned>> *>(context);++**count;delete count;
    });
    xpc_connection_set_event_handler(listener,^(xpc_object_t event){
        if(xpc_get_type(event)==XPC_TYPE_CONNECTION){++*accepted;xpc_connection_cancel(static_cast<xpc_connection_t>(event));}
    });
    xpc_connection_activate(listener);
    auto endpoint=xpc_endpoint_create(listener);
    auto connection=xpc_connection_create_from_endpoint(endpoint);xpc_release(endpoint);
    __block bool prepared=false;
    dispatch_sync(queue,^{
        LaunchNonce nonce{};nonce[0]=1; // no challenge is sent
        NativeVerifierConnection::Callbacks callbacks;
        callbacks.finalized=[finalized,finished]{++*finalized;dispatch_semaphore_signal(finished);};
        auto owner=prepareDevelopmentConnection(connection,queue,helper,nonce,
            verifierMonotonicNS()+2000000000,std::move(callbacks));
        prepared=bool(owner);
        if(owner)owner->cancel();
        else {
            // Factory refusal leaves the borrowed inactive connection with us.
            xpc_connection_set_context(connection,new std::shared_ptr<std::atomic<unsigned>>(finalized));
            xpc_connection_set_finalizer_f(connection,[](void *context){
                auto *count=static_cast<std::shared_ptr<std::atomic<unsigned>> *>(context);++**count;delete count;
            });
            xpc_connection_set_event_handler(connection,^(xpc_object_t){});
            xpc_connection_cancel(connection);xpc_connection_activate(connection);
        }
        xpc_release(connection);
        xpc_connection_cancel(listener);xpc_release(listener);
    });
    // Bounded diagnostic wait; never free resources still reachable by callbacks.
    const auto deadline=verifierMonotonicNS()+3000000000;
    while(finalized->load()!=2 && verifierMonotonicNS()<deadline)
        dispatch_semaphore_wait(finished,dispatch_time(DISPATCH_TIME_NOW,10000000));
    if(finalized->load()!=2)std::_Exit(75);
    dispatch_sync(queue,^{});
    NSDictionary *report=@{@"mode":@"development-connection-preparation",
        @"role":helper?@"helper":@"controller",@"prepared":@(prepared),
        @"connections_finalized":@(finalized->load()),@"accepted_peers":@(accepted->load()),
        @"peer_authenticated":@NO,@"service_mutation_attempted":@NO,@"audio_admitted":@NO};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nullptr];
    dispatch_release(finished);dispatch_release(queue);
    if(!json || fwrite(json.bytes,1,json.length,stdout)!=json.length || fputc('\n',stdout)==EOF)return 74;
    return accepted->load()?3:(prepared?0:2);
}
}
