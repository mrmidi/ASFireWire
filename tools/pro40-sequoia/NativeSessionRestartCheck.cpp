// SPDX-License-Identifier: Apache-2.0
// Hardware check: interrupt one session, then complete another in this process.
#include "NativeSessionRunner.hpp"
#include <charconv>
#include <csignal>
#include <cstdio>
namespace {
volatile std::sig_atomic_t interrupted=0;
void interrupt(int){interrupted=1;}
bool stopRequested(void *) noexcept {return interrupted!=0;}
}
int main(int argc,char **argv) {
    if(argc!=5 || std::string(argv[1])!="--guid")return 64;
    Pro40::NativeSessionOptions options;
    const std::string input=argv[2];
    const auto parsed=std::from_chars(input.data(),input.data()+input.size(),options.guid,16);
    if(parsed.ec!=std::errc{} || parsed.ptr!=input.data()+input.size())return 64;
    options.retainHistory=false;
    options.stop={stopRequested,nullptr};
    // Invalid options must fail without creating a session or touching hardware.
    const auto invalid=Pro40::runNativeSession(options);
    if(invalid.exitCode!=64 || invalid.outcome)return 1;
    std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
    for(unsigned attempt=0;attempt<2;++attempt) {
        options.logPath=argv[3+attempt];
        const auto result=Pro40::runNativeSession(options);
        const auto &outcome=result.outcome;
        std::printf("session=%u exit=%d reason=%s cleanup=%08x accepted=%u\n",attempt,
            result.exitCode,outcome?Pro40::name(outcome->reason):"absent",
            outcome?unsigned(outcome->cleanup):~0u,unsigned(outcome && outcome->accepted()));
        std::fflush(stdout);
        if(!outcome || outcome->cleanup)return 1;
        if(attempt==0) {
            if(result.exitCode!=2 || outcome->reason!=Pro40::StopReason::interrupted)return 1;
            interrupted=0; // First call has fully returned; no active native session.
        } else if(result.exitCode || !outcome->accepted())return 1;
    }
    return 0;
}
