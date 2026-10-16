// SPDX-License-Identifier: Apache-2.0
#include "DuplexProvisionService.hpp"
#include "DuplexProvisionServicePins.hpp" // Generated trusted build configuration.
#include <servers/bootstrap.h>
#include <unistd.h>
#include <cstdio>
#include <csignal>
using namespace Pro40Audio;
namespace {volatile std::sig_atomic_t stopped=0;void stop(int){stopped=1;}}
static auto requirement(CFStringRef text) {
    SecRequirementRef value=nullptr;
    if(SecRequirementCreateWithString(text,0,&value)!=errSecSuccess)return IdentityDetail::Owned<SecRequirementRef>{};
    return IdentityDetail::Owned<SecRequirementRef>(value);
}
int main(int argc,char **argv) {
    if(argc!=2 || std::strcmp(argv[1],"--serve") || geteuid()==0)return 64;
    if(std::signal(SIGTERM,stop)==SIG_ERR || std::signal(SIGINT,stop)==SIG_ERR || std::signal(SIGPIPE,SIG_IGN)==SIG_ERR)return 74;
    auto controller=requirement(CFSTR(PRO40_PROVISION_CONTROLLER_REQUIREMENT));
    auto helper=requirement(CFSTR(PRO40_PROVISION_HELPER_REQUIREMENT));
    if(!controller || !helper)return 65;
    NativeMachPort listener(MACH_PORT_RIGHT_RECEIVE);mach_port_t raw=MACH_PORT_NULL;
    if(bootstrap_check_in(bootstrap_port,PRO40_PROVISION_SERVICE,&raw)!=KERN_SUCCESS || listener.adopt(raw)!=KERN_SUCCESS)return 74;
    DuplexProvisionService service(controller.get(),helper.get());
    std::puts("{\"provision_service_ready\":true,\"native_access\":false}");std::fflush(stdout);
#ifdef PRO40_SESSION_PROFILE
    const std::optional<std::chrono::steady_clock::time_point> deadline=std::nullopt;
#elif defined(PRO40_APPLICATION_TRIAL) || defined(PRO40_ENDURANCE_TRIAL)
    const std::optional deadline=std::chrono::steady_clock::now()+std::chrono::minutes(15);
#else
    const std::optional deadline=std::chrono::steady_clock::now()+std::chrono::seconds(150);
#endif
    while(!stopped && (!deadline || std::chrono::steady_clock::now()<*deadline)) {
        const auto report=service.poll(listener.get());
        if(report.introduced || report.completed || report.denied || report.rejected || report.expired) {
            std::printf("{\"introduced\":%u,\"completed\":%u,\"denied\":%u,\"rejected\":%u,\"expired\":%u,\"error\":%d}\n",
                report.introduced,report.completed,report.denied,report.rejected,report.expired,int(report.rejectedStatus));std::fflush(stdout);
        }
    }
    return listener.close()==KERN_SUCCESS?0:74;
}
