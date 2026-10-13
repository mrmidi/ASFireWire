// SPDX-License-Identifier: Apache-2.0
// Fake runner for testing the actual worker adapter's parent-loss stop query.
#include "../NativeSessionRunner.hpp"
#include <IOKit/IOReturn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
Pro40::NativeSessionResult Pro40::runNativeSession(const NativeSessionOptions &options) {
    FILE *log=std::fopen(options.logPath.c_str(),"wx");
    if(!log)return {1,{}};
    std::fprintf(log,"fixture-entered\n");std::fflush(log);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(!options.stop.requested() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool stopped=options.stop.requested();
    std::fprintf(log,"fixture-cleanup stop=%u\n",unsigned(stopped));
    std::fclose(log);
    return {2,SessionOutcome{stopped?StopReason::interrupted:StopReason::watchdog,
                            stopped?kIOReturnAborted:kIOReturnTimeout,0,
                            std::getenv("PRO40_WORKER_TEST_CLEANUP_FAILURE")?kIOReturnIOError:0}};
}
