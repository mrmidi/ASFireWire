// SPDX-License-Identifier: Apache-2.0
#include "NativeVerifierConnection.hpp"
#include <chrono>
#include <utility>
namespace Pro40Lab {
uint64_t verifierMonotonicNS() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::span<const uint8_t> frameData(xpc_object_t message) {
    if(xpc_get_type(message)!=XPC_TYPE_DICTIONARY)return {};
    auto value=xpc_dictionary_get_value(message,"frame");
    if(!value || xpc_get_type(value)!=XPC_TYPE_DATA)return {};
    const auto size=xpc_data_get_length(value);
    if(size!=24 && size!=Pro40Audio::identityRequestSize && size!=Pro40Audio::identityResultSize)return {};
    return {static_cast<const uint8_t *>(xpc_data_get_bytes_ptr(value)),size};
}
struct NativeVerifierConnection::State : std::enable_shared_from_this<State> {
    dispatch_queue_t queue;
    xpc_connection_t connection=nullptr;
    const bool helper;
    const LaunchNonce nonce;
    const uint64_t deadline;
    const std::string requirement;
    std::optional<NativeProcessBirth> peerInstance;
    VerifierExchange exchange;
    Callbacks callbacks;
    const std::optional<Identity> identity;
    std::optional<VerifierIdentityTransport> work;
    std::optional<VerifierIdentityReply> receipt;
    bool identityComplete=false;
    bool activated=false,closed=false,failed=false;
    State(dispatch_queue_t q,bool h,LaunchNonce n,uint64_t d,std::string r,Callbacks c,std::optional<Identity> i):
        queue(q),helper(h),nonce(n),deadline(d),requirement(std::move(r)),exchange(h,n,d),callbacks(std::move(c)),identity(std::move(i)) {dispatch_retain(queue);}
    ~State(){dispatch_release(queue);}
    void close() {
        if(closed)return;
        closed=true;exchange.cancel();
        if(work)work->cancel();
        if(receipt)receipt->cancel();
        if(connection){
            // SDK connection.h:575-599: cancellation is asynchronous. Context
            // retains State until finalization; pending reply callbacks may run.
            xpc_connection_cancel(connection);
            // SDK connection.h:338-352 forbids dropping the last reference on
            // an inactive created connection. Cancel FIRST, then balance its
            // initial inactive state solely for disposal; never send a frame.
            if(!activated)xpc_connection_activate(connection);
            xpc_release(connection);connection=nullptr;
        }
    }
    void fail(const char *reason) {
        if(closed || failed)return;
        failed=true;close();
        if(callbacks.failure)callbacks.failure(reason);
    }
    bool current() {
        if(closed)return false;
        if(verifierMonotonicNS()>=deadline){fail("deadline-expired");return false;}
        return true;
    }
    std::optional<AuthenticatedVerifierPeer> authenticate(xpc_object_t message){
        const auto birth=authenticatedVerifierBirth(message,xpc_connection_get_pid(connection),requirement);
        if(!birth || (peerInstance && !sameVerifierBirth(*peerInstance,*birth))){fail("peer-instance-unavailable-or-changed");return {};}
        if(!current())return {}; // Security/libproc calls may consume the budget
        peerInstance=birth;return AuthenticatedVerifierPeer{*birth,verifierMonotonicNS()};
    }
    void sendIdentity(){
        if(!current())return;
        receipt.emplace(*identity->request,nonce,deadline);
        auto frame=receipt->begin(true,verifierMonotonicNS());
        if(!frame){fail("identity-request-refused");return;}
        auto message=xpc_dictionary_create(nullptr,nullptr,0);
        xpc_dictionary_set_data(message,"frame",frame->data(),frame->size());
        auto self=shared_from_this();
        xpc_connection_send_message_with_reply(connection,message,queue,^(xpc_object_t response){
            if(!self->current())return;
            if(response==XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT){self->fail("peer-identity-rejected");return;}
            auto peer=self->authenticate(response);if(!peer)return;
            auto result=self->receipt->receive(frameData(response),true,verifierMonotonicNS());
            if(!result){self->fail("identity-result-rejected");return;}
            self->identityComplete=true; // Protocol complete, including a nonzero Security result.
            self->callbacks.identity(*result,*peer); // Does NOT establish cleanup or audio admission.
        });
        xpc_release(message);
    }
    void send(const ExchangeFrame& frame) {
        auto message=xpc_dictionary_create(nullptr,nullptr,0);
        xpc_dictionary_set_data(message,"frame",frame.data(),frame.size());
        auto self=shared_from_this();
        xpc_connection_send_message_with_reply(connection,message,queue,^(xpc_object_t response){
            if(!self->current())return;
            if(response==XPC_ERROR_PEER_CODE_SIGNING_REQUIREMENT){self->fail("peer-identity-rejected");return;}
            const auto peer=self->authenticate(response);if(!peer)return;
            auto accepted=self->exchange.receive(frameData(response),true,verifierMonotonicNS());
            if(!accepted){self->fail("invalid-reply");return;}
            const auto kind=decodeExchange(*accepted)->kind;
            if(self->callbacks.progress)self->callbacks.progress(kind,*peer);
            if(kind==ExchangeKind::ready && self->identity)self->sendIdentity();
        });
        xpc_release(message);
    }
    void event(xpc_object_t request) {
        if(!current())return;
        if(xpc_get_type(request)!=XPC_TYPE_DICTIONARY){
            if(exchange.done())close();else fail("connection-error");return;
        }
        if(!helper){fail("unsolicited-controller-message");return;}
        const auto peer=authenticate(request);if(!peer)return;
        const auto bytes=frameData(request);
        if(identity && bytes.size()==Pro40Audio::identityRequestSize){
            if(!work || identityComplete){fail("identity-request-out-of-order");return;}
            auto answer=work->receive(bytes,true,verifierMonotonicNS,[this]{return closed;},identity->check);
            if(!answer || !current()){if(!closed)fail("identity-work-refused");return;}
            auto response=xpc_dictionary_create_reply(request);
            if(!response){fail("missing-reply-capability");return;}
            identityComplete=true;
            xpc_dictionary_set_data(response,"frame",answer->data(),answer->size());
            xpc_connection_send_message(connection,response);xpc_release(response);return;
        }
        auto incoming=decodeExchange(bytes);
        if(identity && incoming && incoming->kind==ExchangeKind::release && !identityComplete){
            fail("release-before-identity-result");return;
        }
        auto answer=exchange.receive(bytes,true,verifierMonotonicNS());
        if(!answer){fail("invalid-request");return;}
        if(identity && decodeExchange(*answer)->kind==ExchangeKind::ready)
            work.emplace(identity->policy,decodeExchange(*answer)->nonce,deadline);
        auto response=xpc_dictionary_create_reply(request);
        if(!response){fail("missing-reply-capability");return;}
        xpc_dictionary_set_data(response,"frame",answer->data(),answer->size());
        xpc_connection_send_message(connection,response);xpc_release(response);
        // Queued acknowledgement is not delivery, exit, or permission to free
        // resources. Owner must keep the connection alive for peer consumption.
        if(callbacks.progress)callbacks.progress(decodeExchange(*answer)->kind,*peer);
    }
};
NativeVerifierConnection::NativeVerifierConnection(std::shared_ptr<State> s):state_(std::move(s)){}
std::shared_ptr<NativeVerifierConnection> NativeVerifierConnection::create(
    xpc_connection_t c,dispatch_queue_t q,bool helper,LaunchNonce nonce,uint64_t deadline,
    const std::string& requirement,Callbacks callbacks,std::optional<Identity> identity) {
    dispatch_assert_queue(q);
    if(!c || requirement.empty() || deadline<=verifierMonotonicNS())return {};
    if(!helper && !decodeExchange(encodeExchange({ExchangeKind::challenge,nonce})))return {};
    if(identity){
        if(helper){
            if(identity->request || !identity->check ||
               !Pro40Audio::NativeIdentityRequest::nonzero(identity->policy))return {};
        }else if(!identity->request || !identity->request->valid() ||
                 identity->request->requestID!=nonce || identity->check ||
                 Pro40Audio::NativeIdentityRequest::nonzero(identity->policy) || !callbacks.identity)return {};
    }
    // Exactly once, before activation. SDK connection.h:763-794.
    if(xpc_connection_set_peer_code_signing_requirement(c,requirement.c_str()))return {};
    auto s=std::make_shared<State>(q,helper,nonce,deadline,requirement,std::move(callbacks),std::move(identity));
    s->connection=c;xpc_retain(c);
    xpc_connection_set_target_queue(c,q);
    xpc_connection_set_context(c,new std::shared_ptr<State>(s));
    xpc_connection_set_finalizer_f(c,[](void *context){
        auto *owned=static_cast<std::shared_ptr<State> *>(context);
        auto state=*owned;delete owned;
        dispatch_async(state->queue,^{if(state->callbacks.finalized)state->callbacks.finalized();});
    });
    xpc_connection_set_event_handler(c,^(xpc_object_t event){s->event(event);});
    return std::shared_ptr<NativeVerifierConnection>(new NativeVerifierConnection(s));
}
NativeVerifierConnection::~NativeVerifierConnection() {
    auto s=state_;dispatch_async(s->queue,^{s->close();});
}
void NativeVerifierConnection::activate() {
    auto s=state_;dispatch_assert_queue(s->queue);
    if(!s->current())return;
    if(s->activated){s->fail("duplicate-activation");return;}
    s->activated=true;xpc_connection_activate(s->connection);
    if(!s->helper)s->send(encodeExchange({ExchangeKind::challenge,s->nonce}));
}
void NativeVerifierConnection::release(bool allowed) {
    auto s=state_;dispatch_assert_queue(s->queue);
    if(!s->current())return;
    if(!s->activated){s->fail("release-before-activation");return;}
    if(s->identity && !s->identityComplete){s->fail("release-before-identity-result");return;}
    auto message=s->exchange.release(allowed,verifierMonotonicNS());
    if(!message){s->fail("release-not-authorized-or-out-of-order");return;}
    s->send(*message);
}
void NativeVerifierConnection::poll(){auto s=state_;dispatch_assert_queue(s->queue);(void)s->current();}
void NativeVerifierConnection::cancel(){auto s=state_;dispatch_assert_queue(s->queue);s->close();}
}
