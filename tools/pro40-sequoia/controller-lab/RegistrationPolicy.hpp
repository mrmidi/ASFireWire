// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace Pro40Lab {
// Deliberately not numerically coupled to Apple's enum. Adapter must map unknown
// future values to unknown. SDK SMAppService.h:14-33 defines the four known states.
enum class ServiceStatus { notRegistered, enabled, requiresApproval, notFound, unknown };
enum class RegistrationPhase {
    initial, prepared, registering, registrationReturned, unregistering,
    verifyingRemoval, readyToRecord, complete, blocked
};

// Trusted owner assertions, not facts established by this pure policy. The owner
// must bind the actual signed package, raw observations and reviewed provenance
// to the durable intent before authorizeRegistration. Default construction grants
// nothing. A missing journal alone cannot establish first use.
struct FirstRegistrationEvidence {
    bool verifiedDormantPackage = false;
    bool boundIdentityAndObservations = false;
    bool exclusiveEmptyJournal = false;
    bool reviewedFirstUseProvenance = false;
    bool observationsCurrent = false;
    [[nodiscard]] bool complete() const noexcept {
        return verifiedDormantPackage && boundIdentityAndObservations &&
            exclusiveEmptyJournal && reviewedFirstUseProvenance && observationsCurrent;
    }
};

// Pure single-owner policy for one DORMANT registration experiment. No resources,
// service calls or durable storage here. The owner supplies independently obtained
// observations and must retain the package/lock until durable completion.
// No result from this type is a helper-exit receipt or an audio admission decision.
class RegistrationPolicy final {
    RegistrationPhase phase_ = RegistrationPhase::initial;
    bool firstUseExperiment_ = false;
public:
    RegistrationPolicy() = default;
    RegistrationPolicy(const RegistrationPolicy&) = delete;
    RegistrationPolicy& operator=(const RegistrationPolicy&) = delete;
    [[nodiscard]] RegistrationPhase phase() const noexcept { return phase_; }
    [[nodiscard]] bool firstUseExperiment() const noexcept { return firstUseExperiment_; }

    [[nodiscard]] bool prepare(bool exclusiveJournalClear, ServiceStatus status,
                               bool jobIndependentlyAbsent,
                               FirstRegistrationEvidence firstUse = {}) noexcept {
        if (phase_ != RegistrationPhase::initial) return false;
        // Apple DTS thread 719862 describes first-use notFound for login items.
        // Applying it to this agent is an explicit experiment, not a status alias.
        const bool first = status == ServiceStatus::notFound && firstUse.complete();
        if (!exclusiveJournalClear || (status != ServiceStatus::notRegistered && !first) ||
            !jobIndependentlyAbsent) {
            phase_ = RegistrationPhase::blocked;
            return false;
        }
        firstUseExperiment_ = first;
        phase_ = RegistrationPhase::prepared;
        return true;
    }
    // Persist registration intent BEFORE permission to issue the API call.
    [[nodiscard]] bool authorizeRegistration(bool intentDurable) noexcept {
        if (phase_ != RegistrationPhase::prepared) return false;
        phase_ = intentDurable ? RegistrationPhase::registering : RegistrationPhase::blocked;
        return intentDurable;
    }
    // Success, error, approval-required and unknown status all still require
    // cleanup after a mutation was attempted. Owner records NSError/status verbatim.
    [[nodiscard]] bool registrationReturned(bool ownershipConflict = false) noexcept {
        if (phase_ != RegistrationPhase::registering) return false;
        // For example AlreadyRegistered after the preflight: do not remove a
        // registration that another actor may have acquired in the meantime.
        if (ownershipConflict) {
            phase_ = RegistrationPhase::blocked;
            return false;
        }
        phase_ = RegistrationPhase::registrationReturned;
        return true;
    }
    [[nodiscard]] bool authorizeUnregister(bool intentDurable) noexcept {
        if (phase_ != RegistrationPhase::registrationReturned) return false;
        phase_ = intentDurable ? RegistrationPhase::unregistering : RegistrationPhase::blocked;
        return intentDurable;
    }
    // SDK SMAppService.h:215-218: successful callback follows kill, not proof of
    // clean exit. Even an API error is retained for independent removal checks.
    [[nodiscard]] bool unregisterReturned() noexcept {
        if (phase_ != RegistrationPhase::unregistering) return false;
        phase_ = RegistrationPhase::verifyingRemoval;
        return true;
    }
    [[nodiscard]] bool observeRemoval(ServiceStatus status,
                                      bool jobIndependentlyAbsent,
                                      bool unexpectedHelperObserved) noexcept {
        if (phase_ != RegistrationPhase::verifyingRemoval) return false;
        if (status != ServiceStatus::notRegistered || !jobIndependentlyAbsent ||
            unexpectedHelperObserved) {
            phase_ = RegistrationPhase::blocked;
            return false;
        }
        phase_ = RegistrationPhase::readyToRecord;
        return true;
    }
    [[nodiscard]] bool recordCompletion(bool completionDurable) noexcept {
        if (phase_ != RegistrationPhase::readyToRecord) return false;
        phase_ = completionDurable ? RegistrationPhase::complete : RegistrationPhase::blocked;
        return completionDurable;
    }
    // Timeout/cancellation does not cancel an in-flight OS call. Late callbacks
    // cannot reopen this attempt; recovery is separate and must reconcile it.
    void abandon() noexcept {
        if (phase_ != RegistrationPhase::complete) phase_ = RegistrationPhase::blocked;
    }
};
} // namespace Pro40Lab
