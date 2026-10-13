// SPDX-License-Identifier: Apache-2.0
#import <Foundation/Foundation.h>
#include "ControllerConnection.hpp"
#include "DevelopmentConnectionCommand.hpp"
#include <atomic>
namespace Pro40Lab {
int developmentControllerPreparationCommand(){
    auto queue=dispatch_queue_create("pro40.controller-preparation",DISPATCH_QUEUE_SERIAL);
    auto finished=dispatch_semaphore_create(0);
    auto finalizers=std::make_shared<std::atomic<unsigned>>(0);
    auto progress=std::make_shared<std::atomic<unsigned>>(0);
    __block bool prepared=false,pending=false;
    dispatch_sync(queue,^{
        NativeVerifierConnection::Callbacks cb;
        cb.finalized=[finalizers,finished]{++*finalizers;dispatch_semaphore_signal(finished);};
        cb.progress=[progress](ExchangeKind,const AuthenticatedVerifierPeer&){++*progress;};
        cb.failure=[progress](std::string){++*progress;};
        LaunchNonce nonce{};nonce[0]=1;
        auto result=prepareControllerConnection((__bridge void *)queue,nonce,verifierMonotonicNS()+2000000000,std::move(cb));
        prepared=bool(result.connection);pending=result.cleanupPending;
        if(result.connection)result.connection->cancel(); // never activate for exchange
    });
    if(pending && dispatch_semaphore_wait(finished,dispatch_time(DISPATCH_TIME_NOW,3000000000)))std::_Exit(75);
    dispatch_sync(queue,^{});
    NSDictionary *report=@{@"mode":@"development-controller-preparation",@"prepared":@(prepared),
        @"cleanup_obligation_created":@(pending),@"connections_finalized":@(finalizers->load()),
        @"exchange_events":@(progress->load()),@"launch_authorized":@NO,@"audio_admitted":@NO,
        @"service_mutation_attempted":@NO};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nullptr];
    if(!json || fwrite(json.bytes,1,json.length,stdout)!=json.length || fputc('\n',stdout)==EOF)return 74;
    if(progress->load() || finalizers->load()!=(pending?1U:0U))return 3;
    return prepared?0:2;
}
}
