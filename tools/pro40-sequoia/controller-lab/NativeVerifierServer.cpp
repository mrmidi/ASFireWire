// SPDX-License-Identifier: Apache-2.0
#include "NativeVerifierServer.hpp"
namespace Pro40Lab {
struct NativeVerifierServer::State : std::enable_shared_from_this<State> {
    dispatch_queue_t queue;
    xpc_connection_t listener=nullptr;
    dispatch_source_t timer=nullptr;
    std::shared_ptr<NativeVerifierConnection> peer;
    uint64_t deadline;
    std::string requirement, failure;
    Completion completion;
    const std::optional<NativeVerifierConnection::Identity> identity;
    unsigned pending=1;
    bool active=false,closing=false,accepted=false,released=false,finished=false;
    State(dispatch_queue_t q,uint64_t d,std::string r,Completion c,std::optional<NativeVerifierConnection::Identity> i):
        queue(q),deadline(d),requirement(std::move(r)),completion(std::move(c)),identity(std::move(i)){dispatch_retain(q);}
    ~State(){dispatch_release(queue);}
    void finalized(){
        dispatch_assert_queue(queue);
        --pending;
        if(closing && !pending && !finished){
            finished=true;
            auto callback=std::move(completion);
            if(callback)callback(released && failure.empty(),failure);
        }
    }
    void close(std::string reason){
        if(!reason.empty() && failure.empty())failure=std::move(reason);
        if(closing)return;
        closing=true;
        if(timer){dispatch_source_cancel(timer);dispatch_release(timer);timer=nullptr;}
        if(peer){peer->cancel();peer.reset();}
        if(listener){
            xpc_connection_cancel(listener);
            if(!active)xpc_connection_activate(listener);
            xpc_release(listener);listener=nullptr;
        }
    }
    void dispose(xpc_connection_t connection){
        // Track rejected connections too: completion cannot outrun their finalizers.
        ++pending;
        xpc_connection_set_context(connection,new std::shared_ptr<State>(shared_from_this()));
        xpc_connection_set_finalizer_f(connection,[](void *p){
            auto s=*static_cast<std::shared_ptr<State> *>(p);
            delete static_cast<std::shared_ptr<State> *>(p);
            dispatch_async(s->queue,^{s->finalized();});
        });
        xpc_connection_set_event_handler(connection,^(xpc_object_t){});
        xpc_connection_cancel(connection);xpc_connection_activate(connection);
    }
    void event(xpc_object_t event){
        if(xpc_get_type(event)!=XPC_TYPE_CONNECTION){if(!closing)close("listener-error");return;}
        auto connection=static_cast<xpc_connection_t>(event);
        if(closing || accepted || verifierMonotonicNS()>=deadline){
            dispose(connection);close("unexpected-peer-or-expired");return;
        }
        accepted=true;
        auto s=shared_from_this();
        NativeVerifierConnection::Callbacks callbacks;
        callbacks.progress=[s](ExchangeKind kind,const AuthenticatedVerifierPeer&){
            if(kind==ExchangeKind::released)s->released=true;
            // ACK is queued, not delivered. Wait for controller disconnect.
        };
        callbacks.failure=[s](std::string reason){s->close(std::move(reason));};
        callbacks.finalized=[s]{
            s->peer.reset();
            s->close(s->released?"":"peer-closed-before-release");
            s->finalized();
        };
        peer=NativeVerifierConnection::create(connection,queue,true,{},deadline,requirement,std::move(callbacks),identity);
        if(!peer){dispose(connection);close("peer-preparation-failed");return;}
        ++pending;peer->activate();
    }
};
NativeVerifierServer::NativeVerifierServer(std::shared_ptr<State> s):state_(std::move(s)){}
std::shared_ptr<NativeVerifierServer> NativeVerifierServer::create(xpc_connection_t listener,
    dispatch_queue_t queue,uint64_t deadline,std::string requirement,Completion completion,
    std::optional<NativeVerifierConnection::Identity> identity){
    dispatch_assert_queue(queue);
    if(!listener || requirement.empty() || deadline<=verifierMonotonicNS())return {};
    if(identity && (identity->request || !identity->check ||
       !Pro40Audio::NativeIdentityRequest::nonzero(identity->policy)))return {};
    auto s=std::make_shared<State>(queue,deadline,std::move(requirement),std::move(completion),std::move(identity));
    s->listener=listener;xpc_retain(listener);
    xpc_connection_set_target_queue(listener,queue);
    xpc_connection_set_context(listener,new std::shared_ptr<State>(s));
    xpc_connection_set_finalizer_f(listener,[](void *p){
        auto state=*static_cast<std::shared_ptr<State> *>(p);
        delete static_cast<std::shared_ptr<State> *>(p);
        dispatch_async(state->queue,^{state->finalized();});
    });
    xpc_connection_set_event_handler(listener,^(xpc_object_t event){s->event(event);});
    return std::shared_ptr<NativeVerifierServer>(new NativeVerifierServer(s));
}
NativeVerifierServer::~NativeVerifierServer(){auto s=state_;dispatch_async(s->queue,^{if(!s->finished)s->close("owner-destroyed");});}
void NativeVerifierServer::activate(){
    auto s=state_;dispatch_assert_queue(s->queue);
    if(s->closing)return;
    if(s->active){s->close("duplicate-activation");return;}
    if(verifierMonotonicNS()>=s->deadline){s->close("deadline-expired");return;}
    s->active=true;
    s->timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,s->queue);
    // Timer cancellation is itself a tracked local cleanup obligation.
    ++s->pending;
    dispatch_source_set_cancel_handler(s->timer,^{s->finalized();});
    dispatch_source_set_event_handler(s->timer,^{if(verifierMonotonicNS()>=s->deadline)s->close("deadline-expired");});
    dispatch_source_set_timer(s->timer,dispatch_time(DISPATCH_TIME_NOW,0),10000000,1000000);
    dispatch_resume(s->timer);xpc_connection_activate(s->listener);
}
void NativeVerifierServer::cancel(){auto s=state_;dispatch_assert_queue(s->queue);if(!s->finished)s->close("cancelled");}
}
