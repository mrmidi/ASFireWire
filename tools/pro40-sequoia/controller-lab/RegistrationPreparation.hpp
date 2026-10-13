// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationIntent.hpp"
#include <chrono>

namespace Pro40Lab {
struct CollectedRegistrationEvidence {
    NSDictionary *identity = nil;
    NSDictionary *preflight = nil;
};
enum class PreparationResult {
    refused, journalUnavailable, stoppedBeforeIntent, expiredBeforeIntent,
    invalidEvidence, intentFailed, stoppedAfterIntent, expiredAfterIntent, entered
};
// Single owner for the lock, fresh collector and one synchronous handoff. No OS
// service APIs here. Retain this object through eventual operation/cleanup.
// The continuation must issue its operation immediately, not enqueue stale work.
class RegistrationPreparation final {
    RegistrationJournal journal_;
    RegistrationIntent intent_{journal_};
    bool consumed_ = false;
    bool handedOff_ = false;
public:
    using Clock = std::chrono::steady_clock;
    RegistrationPreparation() = default;
    RegistrationPreparation(const RegistrationPreparation&) = delete;
    RegistrationPreparation& operator=(const RegistrationPreparation&) = delete;
    [[nodiscard]] bool complete() const noexcept { return intent_.complete(); }
    void abandon() noexcept { consumed_ = true; intent_.abandon(); }
    [[nodiscard]] bool recordRegistrationResult(NSDictionary *result) {
        return handedOff_ && intent_.recordRegistrationResult(result);
    }
    [[nodiscard]] bool authorizeUnregister() { return handedOff_ && intent_.authorizeUnregister(); }
    [[nodiscard]] bool recordUnregisterResult(NSDictionary *result) {
        return handedOff_ && intent_.recordUnregisterResult(result);
    }
    [[nodiscard]] bool recordRemoval(NSDictionary *evidence) {
        return handedOff_ && intent_.recordRemoval(evidence);
    }
#ifdef PRO40_JOURNAL_TESTING
    void failWriteAt(int stage) { journal_.failWriteStage = stage; }
#endif
    template<class Collector, class Enter, class Now, class Stop>
    [[nodiscard]] PreparationResult run(const std::string& path, NSString *attempt,
        Collector&& collect, Enter&& enter, Now&& now, Stop&& stop) {
        if (consumed_) return PreparationResult::refused;
        consumed_ = true; // Reentrancy is refused before any callback/resource work.
        const auto started = now();
        auto expired = [&] {
            auto current = now();
            return current < started || current - started >= std::chrono::seconds(5);
        };
        if (stop()) return PreparationResult::stoppedBeforeIntent;
        if (expired()) return PreparationResult::expiredBeforeIntent;
        if (!journal_.open(path) || !journal_.requireClear())
            return PreparationResult::journalUnavailable;
        if (stop()) return PreparationResult::stoppedBeforeIntent;
        if (expired()) return PreparationResult::expiredBeforeIntent;
        // No external saved report input: invoke the collector now, under lock.
        auto evidence = collect();
        if (stop()) return PreparationResult::stoppedBeforeIntent;
        if (expired()) return PreparationResult::expiredBeforeIntent;
        if (!RegistrationJournal::validatePreflight(evidence.identity, evidence.preflight))
            return PreparationResult::invalidEvidence;
        // Copy before persistence so a later callback cannot mutate saved inputs.
        NSDictionary *pair = @{@"identity": evidence.identity, @"preflight": evidence.preflight};
        if (![NSJSONSerialization isValidJSONObject:pair]) return PreparationResult::invalidEvidence;
        NSData *bytes = [NSJSONSerialization dataWithJSONObject:pair options:0 error:nullptr];
        NSDictionary *copy = bytes ? [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nullptr] : nil;
        if (!copy) return PreparationResult::invalidEvidence;
        const auto observedAt = now();
        if (observedAt < started || observedAt - started >= std::chrono::seconds(5))
            return PreparationResult::expiredBeforeIntent;
        NSMutableDictionary *preflight = [copy[@"preflight"] mutableCopy];
        // Retain collector's assertions; replace only duration with this owner's
        // measured total interval, including lock acquisition and collection.
        preflight[@"elapsed_ns"] = @(std::chrono::duration_cast<std::chrono::nanoseconds>(
            observedAt - started).count());
        if (!RegistrationJournal::validatePreflight(copy[@"identity"], preflight))
            return PreparationResult::invalidEvidence;
        if (stop()) return PreparationResult::stoppedBeforeIntent;
        if (expired()) return PreparationResult::expiredBeforeIntent;
        if (!intent_.persistBeforeRegistration(attempt, copy[@"identity"], preflight))
            return PreparationResult::intentFailed;
        // fsync/rename can outlast the budget. Keep that intent unresolved and
        // refuse mutation; never delete it or manufacture a cleanup result.
        if (stop()) { intent_.abandon(); return PreparationResult::stoppedAfterIntent; }
        if (expired()) { intent_.abandon(); return PreparationResult::expiredAfterIntent; }
        handedOff_ = true;
        enter(); // Synchronous boundary, lock still held; not a clean-exit receipt.
        return PreparationResult::entered;
    }
};
}
