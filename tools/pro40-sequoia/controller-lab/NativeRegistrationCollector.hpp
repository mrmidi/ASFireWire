// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationPreparation.hpp"
#include <utility>
#include "OriginalControllerProbe.hpp"
#include "LaunchRecoveryReviewOwner.hpp"
namespace Pro40Lab {
// Fresh read-only collection. Never acquires a journal lock itself, so the owner
// may call this with its stable lock already held without recursive acquisition.
[[nodiscard]] NSDictionary *collectNativeRegistrationReport();
// On-demand profile preflight; read-only and never registration permission alone.
[[nodiscard]] NSDictionary *collectNativeVerifierRegistrationReport();
// Fresh removal snapshot bound to this attempt's exact package/process identity.
[[nodiscard]] NSDictionary *collectNativeRemovalReport(NSDictionary *expectedIdentity);
// Read-only removal validation for the on-demand profile. Does not relax dormant
// registration preflight or authorize on-demand registration.
[[nodiscard]] NSDictionary *collectNativeVerifierRemovalReport(NSDictionary *expectedIdentity);
[[nodiscard]] LaunchRecoveryObservation collectNativeLaunchRecoveryObservation();
template<class Stop>
[[nodiscard]] RecoveryResult recoverNativeRemoval(RegistrationRecoveryOwner& owner,
    const std::string& path, NSString *attempt, NSString *hash, Stop&& stop) {
    return owner.run(path, attempt, hash, probeOriginalController,
        [] { return collectNativeRegistrationReport()[@"intent_candidate"]; },
        [] { return RegistrationRecoveryOwner::Clock::now(); }, std::forward<Stop>(stop));
}
template<class Enter, class Stop>
[[nodiscard]] PreparationResult prepareNativeRegistration(
    RegistrationPreparation& owner, const std::string& journalPath, NSString *attempt,
    Enter&& enter, Stop&& stop) {
    return owner.run(journalPath, attempt, [] {
        NSDictionary *report = collectNativeRegistrationReport();
        NSDictionary *candidate = report[@"intent_candidate"];
        return CollectedRegistrationEvidence{candidate[@"identity"], candidate};
    }, std::forward<Enter>(enter),
    [] { return RegistrationPreparation::Clock::now(); }, std::forward<Stop>(stop));
}
}
