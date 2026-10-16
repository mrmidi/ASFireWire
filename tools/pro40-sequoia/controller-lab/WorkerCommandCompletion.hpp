// SPDX-License-Identifier: Apache-2.0
#pragma once
#import <Foundation/Foundation.h>
#include "WorkerCaptureOwner.hpp"
#include "ManagedRunOwner.hpp"
#include "RecordingExportEvidence.hpp"
#include "WorkerSocketCaptureOwner.hpp"
#include "WorkerDuplexConnectionOwner.hpp"
#include "../NativeServiceAttemptResult.hpp"
namespace Pro40Lab {
// Owner-queue reporting join, not a resource owner or authorization mechanism.
// Caller retains all supervisors/owners. Only this join's report reaches the
// command envelope; raw verifier terminal reports must stay inside the command.
template<class CaptureReport, bool Timed=false> class BasicWorkerCommandCompletion final {
    dispatch_queue_t queue_;
    std::function<void(NSDictionary *)> report_;
    NSDictionary *verifier_=nil,*last_=nil;
    std::optional<WorkerStartupReport> worker_;
    std::optional<CaptureReport> capture_;
    std::optional<Pro40Audio::NativeServiceAttemptResult> attempt_;
    std::optional<ManagedRunReport> timing_;
    RecordingExportReport recording_;
    const bool duplexRequired_;
    std::optional<WorkerDuplexReport> duplex_;
    bool entryResolved_=false,finished_=false,stopped_=false;
    void publish(){
        if(finished_)return;
        const bool verifierDone=verifier_ && [verifier_[@"supervision_finished"] boolValue] &&
            [verifier_[@"local_cleanup_complete"] boolValue];
        const bool workerDone=worker_ && worker_->finished && worker_->resourcesComplete && !worker_->setupCleanupError;
        const bool captureDone=capture_ && capture_->endpointsClosed;
        const bool timingDone=!Timed || (timing_ && timing_->timing.complete);
        const bool timingEligible=!Timed || (timing_ && timing_->timing.resultEligible && !timing_->schedulingFailed);
        const bool duplexDone=!duplexRequired_ || (duplex_ && duplex_->finished && duplex_->localClosed);
        const bool done=verifierDone && workerDone && captureDone && entryResolved_ && timingDone && duplexDone;
        NSMutableDictionary *value=verifier_?[verifier_ mutableCopy]:[NSMutableDictionary dictionary];
        value[@"mode"]=@"worker-command";
        value[@"phase"]=done?@"worker-command-finished":@"worker-command-pending";
        value[@"verifier_supervision_finished"]=@(verifierDone);
        value[@"worker_cleanup_complete"]=@(workerDone);
        value[@"capture_endpoints_closed"]=@(captureDone);
        value[@"worker_entry_resolved"]=@(entryResolved_);
        if constexpr(Timed){
            value[@"managed_timing_complete"]=@(timingDone);
            value[@"managed_timing_result_eligible"]=@(timingEligible);
            value[@"managed_timing_scheduling_failed"]=@(timing_ && timing_->schedulingFailed);
            value[@"managed_timing_cause"]=@(timing_?static_cast<unsigned>(timing_->timing.cause):0);
            value[@"managed_cleanup_overdue"]=@(timing_ && timing_->timing.cleanupOverdue);
            value[@"managed_command_overdue"]=@(timing_ && timing_->timing.commandOverdue);
        }
        value[@"worker_startup_error"]=@(worker_?worker_->startupError:0);
        value[@"worker_setup_cleanup_error"]=@(worker_?worker_->setupCleanupError:0);
        value[@"command_stop_observed"]=@(stopped_ || (worker_ && worker_->stopRequested));
        value[@"supervision_finished"]=@(done);
        value[@"local_cleanup_complete"]=@(verifierDone && workerDone && captureDone && timingDone && duplexDone);
        if(duplexRequired_) {
            value[@"duplex_finished"]=@(duplex_ && duplex_->finished); // Snapshot is published only after audio-owner join.
            value[@"duplex_local_cleanup_complete"]=@(duplexDone);
            value[@"duplex_prepared"]=@(duplex_ && duplex_->admitted);
            value[@"duplex_stream_passed"]=@(duplex_ && duplex_->streamPassed);
            value[@"duplex_peer_cleanup_confirmed"]=@(duplex_ && duplex_->peerClean);
            value[@"duplex_cancelled"]=@(duplex_ && duplex_->cancelled);
            if(duplex_)value[@"duplex_connection_diagnostic"]=@{@"failed":@(duplex_->diagnostic.failed),
                @"output":@(unsigned(duplex_->diagnostic.output)),@"clock":@(unsigned(duplex_->diagnostic.clock)),
                @"sequence":@(duplex_->diagnostic.sequence),@"output_admission":@(duplex_->diagnostic.outputAdmission),
                @"input_failure":@(duplex_->diagnostic.inputFailure),@"input_clock_failure":@(duplex_->diagnostic.inputClockFailure),
                @"input_oldest":@(duplex_->diagnostic.inputOldest),@"input_next":@(duplex_->diagnostic.inputNext)};
            if(duplex_) {
                const auto qos=[](const Pro40Audio::AudioThreadScheduling &s)->NSDictionary * {
                    return @{@"accepted":@(s.accepted()),@"before":@(unsigned(s.before)),@"after":@(unsigned(s.after)),
                        @"read_before":@(s.readBefore),@"set":@(s.set),@"read_after":@(s.readAfter)};
                };
                value[@"duplex_scheduling"]=@{@"render":qos(duplex_->renderScheduling),@"connection":qos(duplex_->connectionScheduling)};
            }
        }
        value[@"registration_reconciled"]=@([verifier_[@"registration_reconciled"] boolValue]);
        value[@"worker_attempt_accepted"]=@(done && timingEligible && !stopped_ && !worker_->stopRequested &&
            !worker_->startupError && capture_->status==CaptureReport::Status::captured &&
            capture_->capture && attempt_ && attempt_->accepted());
        // Report collected native evidence separately from process cleanup and
        // signed release. Export evidence is checked separately after child cleanup.
        const auto *collection=worker_ && worker_->collection?&*worker_->collection:nullptr;
        const auto *result=collection && collection->cleanupComplete && collection->collectionFinished &&
            !collection->schedulingFailed && collection->fault==Pro40::WorkerResultCollector::Fault::none &&
            collection->result?&*collection->result:nullptr;
        value[@"worker_collection_fault"]=collection?@(static_cast<unsigned>(collection->fault)):[NSNull null];
        value[@"worker_result_observed"]=@(result!=nullptr);
        value[@"worker_result_exit"]=result?@(result->exitCode):[NSNull null];
        value[@"native_outcome_observed"]=@(result && result->outcome);
        value[@"native_result_accepted"]=@(result && result->accepted());
        value[@"native_cleanup_status"]=result && result->outcome?@(result->outcome->cleanup):[NSNull null];
        if(result && result->outcome){
            const auto &o=*result->outcome;
            value[@"native_outcome"]=@{@"reason":[NSString stringWithUTF8String:Pro40::name(o.reason)],
                @"transfer":@(o.transfer),@"evidence":@(o.evidence),@"cleanup":@(o.cleanup)};
        }else value[@"native_outcome"]=[NSNull null];
        value[@"recording_export_verified"]=@(recording_.verified);
        value[@"recording_export_status"]=[NSString stringWithUTF8String:recording_.status];
        value[@"recording_export_frames"]=@(recording_.frames);
        // This reporting integration never enables an audio path.
        value[@"audio_admitted"]=@NO;
        if([last_ isEqualToDictionary:value])return;
        last_=[value copy];finished_=done;
        auto report=report_;if(done)report_={};report(last_);
    }
public:
    BasicWorkerCommandCompletion(dispatch_queue_t queue,std::function<void(NSDictionary *)> report,bool duplexRequired=false):queue_(queue),report_(std::move(report)),duplexRequired_(duplexRequired){}
    BasicWorkerCommandCompletion(const BasicWorkerCommandCompletion&)=delete;
    BasicWorkerCommandCompletion& operator=(const BasicWorkerCommandCompletion&)=delete;
    void verifierReport(NSDictionary *report){
        dispatch_assert_queue(queue_);if(finished_ || !report)return;
        // Preserve first terminal verifier evidence; never reopen/replace it.
        if(verifier_ && [verifier_[@"supervision_finished"] boolValue])return;
        verifier_=[report copy];publish();
    }
    void workerReport(WorkerStartupReport report){
        dispatch_assert_queue(queue_);if(finished_)return;
        if(worker_){
            if(worker_->startupError)report.startupError=worker_->startupError;
            if(worker_->setupCleanupError)report.setupCleanupError=worker_->setupCleanupError;
            report.stopRequested|=worker_->stopRequested;
        }
        worker_=std::move(report);publish();
    }
    void captureReport(CaptureReport report){
        dispatch_assert_queue(queue_);if(finished_ || capture_ || report.status==CaptureReport::Status::waiting)return;
        capture_=std::move(report);publish();
    }
    // Explicitly resolve even when verification was refused and no entry exists.
    // Never infer a runner result from successful process or endpoint cleanup.
    [[nodiscard]] bool resolveEntry(std::optional<Pro40Audio::NativeServiceAttemptResult> attempt,RecordingExportReport recording={}){
        dispatch_assert_queue(queue_);if(finished_ || entryResolved_)return false;
        entryResolved_=true;recording_=recording;
        if(attempt)attempt_.emplace(*attempt);
        publish();return true;
    }
    void timingReport(ManagedRunReport report) requires(Timed){
        dispatch_assert_queue(queue_);if(finished_ || (timing_ && timing_->timing.complete))return;
        if(timing_){
            report.schedulingFailed|=timing_->schedulingFailed;
            report.timing.cleanupOverdue|=timing_->timing.cleanupOverdue;
            report.timing.commandOverdue|=timing_->timing.commandOverdue;
            if(timing_->timing.cause!=Pro40::ManagedRunTiming::Cause::none)report.timing.cause=timing_->timing.cause;
        }
        if(report.schedulingFailed || report.timing.cause!=Pro40::ManagedRunTiming::Cause::none)report.timing.resultEligible=false;
        timing_=report;publish();
    }
    void observeStop(){dispatch_assert_queue(queue_);if(!finished_){stopped_=true;publish();}}
    void duplexReport(WorkerDuplexReport report){dispatch_assert_queue(queue_);if(!finished_ && duplexRequired_ && report.finished && !duplex_){duplex_=report;publish();}}
    bool finished()const{dispatch_assert_queue(queue_);return finished_;}
};
using WorkerCommandCompletion=BasicWorkerCommandCompletion<WorkerCaptureReport>;
using WorkerSocketCommandCompletion=BasicWorkerCommandCompletion<WorkerSocketCaptureReport>;
}
