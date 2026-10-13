// SPDX-License-Identifier: Apache-2.0
// Finite diagnostic sequence; each call fully closes before the next starts.
#include "SessionSequence.hpp"
#include <charconv>
#include <csignal>
#include <cstdio>
namespace {
volatile std::sig_atomic_t interrupted=0;
void interrupt(int){interrupted=1;}
bool stopRequested(void *) noexcept {return interrupted!=0;}
}
int main(int argc,char **argv) {
    if(argc<4 || argc>11 || std::string(argv[1])!="--guid")return 64;
    Pro40::NativeSessionOptions options;
    const std::string input=argv[2];
    const auto parsed=std::from_chars(input.data(),input.data()+input.size(),options.guid,16);
    if(parsed.ec!=std::errc{} || parsed.ptr!=input.data()+input.size())return 64;
    options.retainHistory=false;options.stop={stopRequested,nullptr};
    std::array<std::string,Pro40::SessionSequenceResult::maximum> paths;
    for(int i=3;i<argc;++i)paths[i-3]=argv[i];
    std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
    const auto result=Pro40::runSessionSequence(options,
        std::span<const std::string>(paths.data(),argc-3),Pro40::runNativeSession);
    for(unsigned i=0;i<result.attempted;++i) {
        const auto &r=result.sessions[i];
        std::printf("sequence-session=%u exit=%d reason=%s cleanup=%08x accepted=%u\n",
            i,r.exitCode,r.outcome?Pro40::name(r.outcome->reason):"absent",
            r.outcome?unsigned(r.outcome->cleanup):~0u,unsigned(r.accepted()));
    }
    std::printf("sequence attempted=%u requested=%d stop=%s completed=%u\n",result.attempted,
        argc-3,Pro40::name(result.stop),unsigned(result.stop==Pro40::SequenceStop::completed));
    if(result.stop==Pro40::SequenceStop::invalidOptions)return 64;
    return result.stop==Pro40::SequenceStop::completed?0:2;
}
