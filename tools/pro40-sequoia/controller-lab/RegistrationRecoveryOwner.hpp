// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationJournal.hpp"
#include <chrono>
#include <utility>
namespace Pro40Lab {
enum class OriginalControllerState { unknown, sameInstance, differentInstance, absent };
enum class RecoveryResult { refused, unavailable, stopped, expired, ownerUnresolved,
                            invalidObservation, persistenceFailed, recovered, eligible };
// Explicit record-only recovery. No service mutations. Callbacks run under the
// retained exclusive lock; original positive removal is required before probing.
class RegistrationRecoveryOwner final {
    RegistrationJournal journal_;
    bool consumed_ = false;
public:
    using Clock = std::chrono::steady_clock;
#ifdef PRO40_JOURNAL_TESTING
    void failWriteAt(int stage) { journal_.failWriteStage = stage; }
#endif
private:
    template<class Probe, class Collect, class Now, class Stop>
    [[nodiscard]] RecoveryResult inspect(const std::string& path, NSString *attempt, NSString *hash,
                                    Probe&& probe, Collect&& collect, Now&& now, Stop&& stop, bool preview) {
        if (consumed_) return RecoveryResult::refused;
        consumed_ = true;
        const auto start = now();
        auto expired = [&] {
            const auto current = now();
            return current < start || current - start >= std::chrono::seconds(5);
        };
        if (stop()) return RecoveryResult::stopped;
        if (expired()) return RecoveryResult::expired;
        if (!journal_.open(path, preview, true)) return RecoveryResult::unavailable;
        NSDictionary *record = journal_.reviewRemoval(attempt, hash);
        if (!record) return RecoveryResult::unavailable;
        if (stop()) return RecoveryResult::stopped;
        if (expired()) return RecoveryResult::expired;
        const auto state = probe(record[@"metadata"]);
        if (state != OriginalControllerState::absent && state != OriginalControllerState::differentInstance)
            return RecoveryResult::ownerUnresolved;
        if (stop()) return RecoveryResult::stopped;
        if (expired()) return RecoveryResult::expired;
        NSDictionary *observation = collect();
        if (stop()) return RecoveryResult::stopped;
        if (expired()) return RecoveryResult::expired;
        if (![observation isKindOfClass:[NSDictionary class]] ||
            !RegistrationJournal::validatePreflight(observation[@"identity"], observation))
            return RecoveryResult::invalidObservation;
        NSMutableDictionary *fresh = [observation mutableCopy];
        const auto observed = now();
        if (observed < start || observed - start >= std::chrono::seconds(5))
            return RecoveryResult::expired;
        fresh[@"elapsed_ns"] = @(std::chrono::duration_cast<std::chrono::nanoseconds>(observed - start).count());
        // Narrow inference: the stored normal protocol reached positive removal,
        // the original controller is gone, and no competing owner holds the lock.
        // This does not establish helper clean exit or cover earlier crash stages.
        NSDictionary *evidence = @{@"record_sha256": hash, @"observation": fresh,
            @"original_owner_gone": @YES, @"no_inflight_operations": @YES,
            @"review_reference": state == OriginalControllerState::absent ?
                @"final-removal-v1: recorded positive removal; original PID absent; locked fresh corroboration" :
                @"final-removal-v1: recorded positive removal; different process instance; locked fresh corroboration"};
        if (stop()) return RecoveryResult::stopped;
        if (expired()) return RecoveryResult::expired;
        if (!RegistrationJournal::validateRecovery(record, evidence)) return RecoveryResult::invalidObservation;
        if (preview) return RecoveryResult::eligible;
        // No OS mutation follows. If fsync runs late, report persisted result;
        // deadline is checked before writing, never used to falsify stored state.
        return journal_.recoverRemoval(attempt, hash, evidence) ?
            RecoveryResult::recovered : RecoveryResult::persistenceFailed;
    }
public:
    const std::string& error() const { return journal_.error; }
    template<class Probe, class Collect, class Now, class Stop>
    [[nodiscard]] RecoveryResult run(const std::string& path, NSString *attempt, NSString *hash,
                                    Probe&& probe, Collect&& collect, Now&& now, Stop&& stop) {
        return inspect(path, attempt, hash, std::forward<Probe>(probe), std::forward<Collect>(collect),
            std::forward<Now>(now), std::forward<Stop>(stop), false);
    }
    template<class Probe, class Collect, class Now, class Stop>
    [[nodiscard]] RecoveryResult preview(const std::string& path, NSString *attempt, NSString *hash,
                                        Probe&& probe, Collect&& collect, Now&& now, Stop&& stop) {
        return inspect(path, attempt, hash, std::forward<Probe>(probe), std::forward<Collect>(collect),
            std::forward<Now>(now), std::forward<Stop>(stop), true);
    }

};
}
