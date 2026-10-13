// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeVerifierServer.hpp"
#include <cstdlib>
namespace Pro40Lab {
struct BlockingVerifierServerResult {
    bool prepared=false,completed=false,success=false;
    std::string reason;
};
// Process entry only: never call from the listener queue. The caller supplies
// trusted provisioning and a fixed service name (nullptr for anonymous tests).
// Protocol completion does not imply remote receipt or process exit. Return only
// after the owner reports all local finalizers; an overdue cleanup exits 75.
inline BlockingVerifierServerResult runBlockingVerifierServer(
    const char *service,const std::string& requirement,uint64_t exchangeNS,
    std::optional<NativeVerifierConnection::Identity> identity = {}){
    if(requirement.empty() || !exchangeNS || exchangeNS>5000000000)
        return {false,false,false,"invalid-configuration"};
    auto queue=dispatch_queue_create("pro40.verifier-entry",DISPATCH_QUEUE_SERIAL);
    auto finished=dispatch_semaphore_create(0);
    auto result=std::make_shared<BlockingVerifierServerResult>();
    std::shared_ptr<NativeVerifierServer> owner;auto slot=&owner;
    dispatch_sync(queue,^{
        auto listener=service?xpc_connection_create_mach_service(service,queue,XPC_CONNECTION_MACH_SERVICE_LISTENER):
            xpc_connection_create(nullptr,queue);
        if(!listener)std::_Exit(75);
        *slot=NativeVerifierServer::create(listener,queue,verifierMonotonicNS()+exchangeNS,requirement,
            [result,finished](bool success,std::string reason){
                result->completed=true;result->success=success;result->reason=std::move(reason);
                dispatch_semaphore_signal(finished);
            },identity);
        if(!*slot)std::_Exit(75); // finalization not established; do not unwind
        result->prepared=true;(*slot)->activate();xpc_release(listener);
    });
    if(dispatch_semaphore_wait(finished,dispatch_time(DISPATCH_TIME_NOW,exchangeNS+2000000000)))std::_Exit(75);
    dispatch_sync(queue,^{slot->reset();});
    dispatch_sync(queue,^{});
    dispatch_release(finished);dispatch_release(queue);
    return *result;
}
}
