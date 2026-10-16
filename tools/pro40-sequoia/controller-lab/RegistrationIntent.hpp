// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationJournal.hpp"
#include "RegistrationPolicy.hpp"

namespace Pro40Lab {
// One-shot bridge between validated durable evidence and in-memory authorization.
// Own neither journal nor OS service. The journal must outlive this object and
// retain its lock through the eventual service operation and cleanup.
class RegistrationIntent final {
    RegistrationJournal& journal_;
    RegistrationPolicy policy_;
    bool consumed_ = false;
public:
    explicit RegistrationIntent(RegistrationJournal& journal) : journal_(journal) {}
    RegistrationIntent(const RegistrationIntent&) = delete;
    RegistrationIntent& operator=(const RegistrationIntent&) = delete;
    [[nodiscard]] bool firstUseExperiment() const noexcept { return policy_.firstUseExperiment(); }
    void abandon() noexcept { policy_.abandon(); }
    [[nodiscard]] bool complete() const noexcept { return policy_.phase() == RegistrationPhase::complete; }
    [[nodiscard]] bool recordRegistrationResult(NSDictionary *result) {
        if (policy_.phase() != RegistrationPhase::registering) return false;
        if (!journal_.append(@"register_result", result)) { abandon(); return false; }
        return policy_.registrationReturned([result[@"ownership_conflict"] boolValue]);
    }
    [[nodiscard]] bool authorizeUnregister() {
        if (policy_.phase() != RegistrationPhase::registrationReturned) return false;
        return policy_.authorizeUnregister(journal_.append(@"unregister_intent", @{}));
    }
    [[nodiscard]] bool recordUnregisterResult(NSDictionary *result) {
        if (policy_.phase() != RegistrationPhase::unregistering) return false;
        if (!journal_.append(@"unregister_result", result)) { abandon(); return false; }
        if ([result[@"ownership_conflict"] boolValue]) { abandon(); return false; }
        return policy_.unregisterReturned();
    }
    [[nodiscard]] bool recordRemoval(NSDictionary *evidence) {
        if (policy_.phase() != RegistrationPhase::verifyingRemoval) return false;
        if (!journal_.append(@"removal", evidence)) { abandon(); return false; }
        if (!policy_.observeRemoval([evidence[@"not_registered"] boolValue] ?
                ServiceStatus::notRegistered : ServiceStatus::unknown,
                [evidence[@"job_absent"] boolValue], [evidence[@"unexpected_helper"] boolValue])) return false;
        return policy_.recordCompletion(journal_.append(@"complete", @{}));
    }
    [[nodiscard]] bool persistBeforeRegistration(NSString *attempt, NSDictionary *identity,
                                               NSDictionary *preflight) {
        if (consumed_) return false;
        consumed_ = true;
        // begin validates identity equality, raw absence output, package/current
        // assertions, provenance and (for first use) empty journal under its lock.
        // It persists that complete snapshot BEFORE any permission is returned.
        if (!journal_.begin(attempt, identity, preflight)) {
            policy_.abandon();
            return false;
        }
        bool first = [preflight[@"experiment"] isEqual:@"reviewed-first-use"];
        FirstRegistrationEvidence evidence{first, first, first, first, first};
        if (!policy_.prepare(true, first ? ServiceStatus::notFound : ServiceStatus::notRegistered,
                             true, evidence)) return false;
        return policy_.authorizeRegistration(true);
    }
};
}
