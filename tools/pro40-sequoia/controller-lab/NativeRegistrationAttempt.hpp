#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeRegistrationCollector.hpp"
#include "RegistrationEventLoop.hpp"
namespace Pro40Lab {
// Construct on the controller's serial queue. No mutation until lifecycle.start.
// Snapshot is shared by the fresh preflight collector and removal observer, not
// loaded from a user-supplied report. Keep lastRemovalReport for diagnostics.
struct NativeAttemptEvidence {
    NSDictionary *identity = nil;
    NSDictionary *lastRemovalReport = nil;
};
struct NativeRegistrationAttempt {
    std::shared_ptr<NativeAttemptEvidence> evidence;
    std::shared_ptr<NativeRegistrationLifecycle> lifecycle;
    NativeRegistrationLifecycle::Collector collect;
};
inline NativeRegistrationAttempt makeNativeRegistrationAttempt(dispatch_queue_t queue) {
    auto evidence = std::make_shared<NativeAttemptEvidence>();
    SMAppService *service = [SMAppService agentServiceWithPlistName:@PRO40_LAB_PLIST];
    auto lifecycle = NativeRegistrationLifecycle::create(service, queue, [evidence]() -> NSDictionary * {
        if (!evidence->identity) return nil;
        evidence->lastRemovalReport = collectNativeRemovalReport(evidence->identity);
        return [evidence->lastRemovalReport[@"valid"] boolValue] ?
            evidence->lastRemovalReport[@"removal_evidence"] : nil;
    });
    return {evidence, lifecycle, [evidence] {
        NSDictionary *candidate = collectNativeRegistrationReport()[@"intent_candidate"];
        evidence->identity = [candidate[@"identity"] copy];
        return CollectedRegistrationEvidence{evidence->identity, candidate};
    }};
}
struct NativeRegistrationExperiment {
    std::shared_ptr<NativeRegistrationEventLoop> loop;
    NativeRegistrationLifecycle::Collector collect;
    std::function<void()> requestStop;
};
inline NativeRegistrationExperiment makeNativeRegistrationExperiment(dispatch_queue_t queue,
    NativeRegistrationEventLoop::Report report) {
    if (!queue || !report) return {};
    auto attempt = makeNativeRegistrationAttempt(queue);
    auto evidence = attempt.evidence;
    auto loop = NativeRegistrationEventLoop::create(queue, attempt.lifecycle,
        [evidence, report = std::move(report)](NSDictionary *status) {
            NSMutableDictionary *full = [status mutableCopy];
            full[@"identity"] = evidence->identity ?: @{};
            full[@"removal_report"] = evidence->lastRemovalReport ?: @{};
            report(full);
        });
    return {loop, std::move(attempt.collect), [owner = attempt.lifecycle] { owner->requestStop(); }};
}

}
