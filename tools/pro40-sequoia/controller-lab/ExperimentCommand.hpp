// SPDX-License-Identifier: Apache-2.0
#pragma once
#import <Foundation/Foundation.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include "CommandWaitTiming.hpp"
namespace Pro40Lab {
// One command per process; handlers do only async-signal-safe scalar assignment.
inline volatile sig_atomic_t experimentSignal = 0;
inline void experimentSignalHandler(int signal) { experimentSignal = signal; }
// Main-thread scoped and nestable so the idle controller and its active command
// share one stop request. Entering the command must not erase an idle SIGTERM.
class ExperimentSignals final {
    inline static bool active_=false;
    struct sigaction oldInt_{},oldTerm_{},oldPipe_{};
    bool owns_=false,ready_=false;
public:
    ExperimentSignals() {
        if(active_){ready_=true;return;}
        struct sigaction action{};action.sa_handler=experimentSignalHandler;sigemptyset(&action.sa_mask);
        experimentSignal=0;
        if(sigaction(SIGINT,&action,&oldInt_))return;
        if(sigaction(SIGTERM,&action,&oldTerm_)){sigaction(SIGINT,&oldInt_,nullptr);return;}
        action.sa_handler=SIG_IGN;
        if(sigaction(SIGPIPE,&action,&oldPipe_)){
            sigaction(SIGINT,&oldInt_,nullptr);sigaction(SIGTERM,&oldTerm_,nullptr);return;
        }
        owns_=ready_=active_=true;
    }
    ~ExperimentSignals(){if(owns_){
        sigaction(SIGINT,&oldInt_,nullptr);sigaction(SIGTERM,&oldTerm_,nullptr);sigaction(SIGPIPE,&oldPipe_,nullptr);active_=false;
    }}
    ExperimentSignals(const ExperimentSignals&)=delete;
    ExperimentSignals& operator=(const ExperimentSignals&)=delete;
    bool ready()const{return ready_;}
};
struct ExperimentControl {
    std::function<void()> start;
    std::function<void()> stop; // must be thread-safe, nonblocking, no queue sync
};
struct ExperimentMailbox {
    std::mutex mutex;
    std::deque<NSDictionary *> reports;
    bool finished = false, reconciled = false, stopping = false;
};
// Host process envelope. Finite wait does not imply bounded SDK completion: an
// unresolved return requires process exit and later journal inspection, not retry.
template<class Factory>
int runExperimentCommand(Factory&& factory, NSString *attempt, NSString *path,
                         std::optional<std::chrono::milliseconds> limit = std::chrono::seconds(15),
                         NSString *mode = @"dormant-registration-command") {
    ExperimentSignals signals;if(!signals.ready())return 71;
    struct Restore {
        int flags = -1;
        ~Restore() {
            if (flags >= 0) fcntl(STDOUT_FILENO, F_SETFL, flags);
        }
    } restore;
    restore.flags = fcntl(STDOUT_FILENO,F_GETFL);
    if (restore.flags < 0 || fcntl(STDOUT_FILENO,F_SETFL,restore.flags|O_NONBLOCK)) return 74;
    auto emit = [](NSDictionary *report) {
        NSData *bytes = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
        if (!bytes || bytes.length > 262144) return false;
        NSMutableData *line = [bytes mutableCopy]; const char newline = '\n'; [line appendBytes:&newline length:1];
        size_t offset = 0;
        while (offset < line.length) {
            ssize_t n = write(STDOUT_FILENO, static_cast<const char *>(line.bytes)+offset, line.length-offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false; // includes pipe backpressure; never wait on the reader
            offset += static_cast<size_t>(n);
        }
        return true;
    };
    if (!emit(@{@"mode":mode, @"phase":@"starting",
        @"attempt":attempt, @"journal_path":path, @"registration_reconciled":@NO})) return 74;
    auto box = std::make_shared<ExperimentMailbox>();
    auto queue = dispatch_queue_create("local.pro40.registration-command", DISPATCH_QUEUE_SERIAL);
    auto control = factory(queue, [box](NSDictionary *r) {
        std::lock_guard lock(box->mutex);
        box->stopping|=[r[@"command_stop_observed"] boolValue] || [r[@"managed_timing_cause"] unsignedIntValue]!=0 ||
            [r[@"managed_timing_complete"] boolValue] || [r[@"worker_cleanup_complete"] boolValue];
        // Event loop emits a fixed small number of phase changes, not telemetry.
        if (box->reports.size() < 8) box->reports.push_back([r copy]);
        else if ([r[@"supervision_finished"] boolValue]) {
            // Joined worker/verifier progress can fill the bounded mailbox.
            // Keep the terminal evidence even when older progress is dropped.
            box->reports.pop_back();box->reports.push_back([r copy]);
        }
        if ([r[@"supervision_finished"] boolValue]) {
            box->finished = true; box->reconciled = [r[@"registration_reconciled"] boolValue];
        }
    });
    if (!control.start || !control.stop) return 71;
    CommandWaitTiming timing(std::chrono::steady_clock::now(),limit);
    dispatch_async(queue, ^{ @autoreleasepool { control.start(); } });
    bool outputFailed = false, stopRequested = false;
    for (;;) {
        if ((experimentSignal || outputFailed) && !stopRequested) { control.stop(); stopRequested = true; }
        std::deque<NSDictionary *> reports; bool finished, reconciled, stopping;
        { std::lock_guard lock(box->mutex); reports.swap(box->reports); finished=box->finished; reconciled=box->reconciled; stopping=box->stopping; }
        for (NSDictionary *report : reports) if (!outputFailed && !emit(report)) outputFailed = true;
        if (finished) return outputFailed ? 74 : reconciled ? 0 : 2;
        if (timing.expired(std::chrono::steady_clock::now(),stopRequested || stopping)) {
            control.stop();
            bool written = !outputFailed && emit(@{@"mode":mode,
                @"phase":@"host-wait-expired-unresolved", @"attempt":attempt, @"journal_path":path,
                @"supervision_finished":@NO, @"registration_reconciled":@NO,
                @"requires_journal_inspection":@YES, @"signal":@(experimentSignal)});
            return written ? 75 : 74;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}
