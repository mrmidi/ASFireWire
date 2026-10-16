// SPDX-License-Identifier: Apache-2.0
#include "OwnershipQuery.hpp"
#include "OwnershipQueryScope.hpp"
#include "OwnershipQueryServerPins.hpp" // Trusted generated build policy.
#include <cstdio>
#include <csignal>
namespace {volatile std::sig_atomic_t stopped=0;void stop(int){stopped=1;}}
int main(int argc,char **argv) {@autoreleasepool {
    using namespace Pro40Audio;using namespace Pro40Lab;
    if(argc!=2 || std::strcmp(argv[1],"--serve") || geteuid()!=0 || PRO40_OWNERSHIP_UID==0)return 64;
    if(std::signal(SIGTERM,stop)==SIG_ERR || std::signal(SIGINT,stop)==SIG_ERR || std::signal(SIGPIPE,SIG_IGN)==SIG_ERR)return 74;
    SecRequirementRef rawRequirement=nullptr;
    if(SecRequirementCreateWithString(CFSTR(PRO40_OWNERSHIP_CONTROLLER_REQUIREMENT),0,&rawRequirement)!=errSecSuccess)return 65;
    IdentityDetail::Owned<SecRequirementRef> requirement(rawRequirement);
    NativeMachPort listener(MACH_PORT_RIGHT_RECEIVE);mach_port_t raw=MACH_PORT_NULL;
    if(bootstrap_check_in(bootstrap_port,PRO40_OWNERSHIP_SERVICE,&raw)!=KERN_SUCCESS || listener.adopt(raw)!=KERN_SUCCESS)return 74;
    std::puts("{\"ownership_query_ready\":true,\"hardware_allowed\":false}");std::fflush(stdout);
#ifdef PRO40_SESSION_PROFILE
    const std::optional<std::chrono::steady_clock::time_point> deadline=std::nullopt;
#elif defined(PRO40_APPLICATION_TRIAL) || defined(PRO40_ENDURANCE_TRIAL)
    const std::optional deadline=std::chrono::steady_clock::now()+std::chrono::minutes(15);
#else
    const std::optional deadline=std::chrono::steady_clock::now()+std::chrono::seconds(150);
#endif
    while(!stopped && (!deadline || std::chrono::steady_clock::now()<*deadline)) {@autoreleasepool {
        auto introduction=ProvisionIntroduction::serveOne(listener.get(),requirement.get(),requirement.get(),20);
        if(!introduction.accepted) {
            if(introduction.transport==KERN_SUCCESS){std::printf("{\"ownership_query_denied\":%d}\n",int(introduction.identity));std::fflush(stdout);}
            continue;
        }
        auto &peer=*introduction.accepted;
        if(peer.role!=ProvisionIntroduction::Role::controller || audit_token_to_euid(peer.token)!=PRO40_OWNERSHIP_UID)continue;
        bool executed=false;auto collected=OwnershipQuery::Status::collectionFailed;
        const auto status=OwnershipQuery::serve(peer.receive.get(),peer.token,PRO40_OWNERSHIP_UID,[&]{
            executed=true;
            auto query=observeProcess({"/usr/bin/sfltool","dumpbtm"},std::chrono::seconds(2),1048576);
            if(!query.usable() || query.exit || !query.err.empty())return OwnershipQuery::Collected{};
            auto scoped=scopedOwnershipText(query.out,PRO40_OWNERSHIP_UID,@PRO40_OWNERSHIP_APP_ID,@PRO40_OWNERSHIP_LABEL);
            if(!scoped){collected=OwnershipQuery::Status::scopeFailed;return OwnershipQuery::Collected{collected,{}};}
            collected=OwnershipQuery::Status::ok;
            return OwnershipQuery::Collected{OwnershipQuery::Status::ok,std::move(*scoped)};
        });
        std::printf("{\"ownership_query_transport\":%d,\"executed\":%s,\"result\":%u}\n",int(status),executed?"true":"false",unsigned(collected));std::fflush(stdout);
    }}
    const auto closed=listener.close();
    std::printf("{\"ownership_query_stopped\":true,\"close_status\":%d}\n",int(closed));std::fflush(stdout);
    return closed==KERN_SUCCESS?0:74;
}}
