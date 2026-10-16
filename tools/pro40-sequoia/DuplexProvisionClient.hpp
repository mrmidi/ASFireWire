// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexProvisionIntroduction.hpp"
#include "ProvisionAvailability.hpp"
#include <thread>
#include <servers/bootstrap.h>
#include <cstdlib>
namespace Pro40Audio::ProvisionClient {
inline uint64_t nonce() {uint64_t value=0;do{arc4random_buf(&value,sizeof(value));}while(!value);return value;}
// Discovery only; connect() independently verifies the code behind the public
// name before a caller transfers its controller endpoint or trusts an intro.
inline ProvisionIntroduction::Answer connect(const char *service,SecRequirementRef expectedServer,ProvisionIntroduction::Role role) {
    ProvisionIntroduction::Answer result;NativeMachPort lookup(MACH_PORT_RIGHT_SEND);mach_port_t raw=MACH_PORT_NULL;
    if(!service || !*service || !expectedServer)return result;
    result.transport=bootstrap_look_up(bootstrap_port,service,&raw);
    if(result.transport!=KERN_SUCCESS)return result;
    result.transport=lookup.adopt(raw);if(result.transport!=KERN_SUCCESS)return result;
    result=ProvisionIntroduction::connect(lookup.get(),expectedServer,role,nonce());
    if(lookup.close()!=KERN_SUCCESS){result.connection.reset();result.transport=KERN_FAILURE;}
    return result;
}
inline DuplexProvisionRegistry::Result registration(const char *service,SecRequirementRef expectedServer,uint64_t registration,
                                                   mach_port_t controllerReceive=MACH_PORT_NULL) {
    auto connection=connect(service,expectedServer,ProvisionIntroduction::Role::controller);
    if(!connection.ok())return DuplexProvisionRegistry::Result::identity;
    auto result=ProvisionTransport::exchange(connection.connection->service.get(),connection.connection->token,
        controllerReceive==MACH_PORT_NULL?ProvisionTransport::Kind::unbind:ProvisionTransport::Kind::bind,registration,controllerReceive);
    return result.transport==KERN_SUCCESS && result.peerMatched?result.result:DuplexProvisionRegistry::Result::transport;
}
inline DuplexProvisionRegistry::Lookup lookupOnce(const char *service,SecRequirementRef expectedServer) {
    auto connection=connect(service,expectedServer,ProvisionIntroduction::Role::helper);
    if(!connection.ok())return {DuplexProvisionRegistry::Result::transport,{}};
    auto result=ProvisionTransport::exchange(connection.connection->service.get(),connection.connection->token,ProvisionTransport::Kind::lookup,nonce());
    if(result.transport!=KERN_SUCCESS || !result.peerMatched)return {DuplexProvisionRegistry::Result::transport,{}};
    return {result.result,std::move(result.introduction)};
}
inline std::unique_ptr<DuplexProvisionRegistry::Introduction> lookup(const char *service,SecRequirementRef expectedServer) {
    auto result=lookupOnce(service,expectedServer);
    return result.result==DuplexProvisionRegistry::Result::ok?std::move(result.introduction):nullptr;
}
inline std::unique_ptr<DuplexProvisionRegistry::Introduction> awaitController(const char *service,SecRequirementRef expectedServer) {
    auto result=awaitProvisionController([&]{return lookupOnce(service,expectedServer);},
        []{return std::chrono::steady_clock::now();},[]{std::this_thread::sleep_for(std::chrono::milliseconds(50));},std::chrono::seconds(5));
    return result.result==DuplexProvisionRegistry::Result::ok?std::move(result.introduction):nullptr;
}
}
