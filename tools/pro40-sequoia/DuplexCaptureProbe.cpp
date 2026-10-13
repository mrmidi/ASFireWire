// SPDX-License-Identifier: Apache-2.0
#include "NativeSessionRunner.hpp"
#include <charconv>
#include <csignal>
#include <string>
static volatile std::sig_atomic_t interrupted=0;
static void interrupt(int) {interrupted=1;}
static bool stopRequested(void *) noexcept {return interrupted!=0;}
int main(int argc,char **argv) {
    Pro40::NativeSessionOptions options;
#ifdef PRO40_DMA_SOURCE_QUEUE
    if(argc==5 && std::string(argv[4])=="--no-history")options.retainHistory=false;
    else if(argc!=4)return 64;
#else
    if(argc!=4)return 64;
#endif
    if(std::string(argv[1])!="--guid")return 64;
    const std::string input=argv[2];
    const auto parsed=std::from_chars(input.data(),input.data()+input.size(),options.guid,16);
    if(parsed.ec!=std::errc{} || parsed.ptr!=input.data()+input.size())return 64;
    options.logPath=argv[3];options.stop={stopRequested,nullptr};
    std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
    return Pro40::runNativeSession(options).exitCode;
}
