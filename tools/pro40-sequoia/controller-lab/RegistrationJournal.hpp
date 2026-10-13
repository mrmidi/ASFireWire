// SPDX-License-Identifier: Apache-2.0
#pragma once
#import <Foundation/Foundation.h>
#include <string>

namespace Pro40Lab {
// Single control owner. Directory is stable across app rebuilds. Recovery is
// explicit and narrow; no automatic deletion. Observations come from trusted owner.
class RegistrationJournal final {
    int directory_ = -1, lock_ = -1;
    bool poisoned_ = false;
    bool readOnly_ = false;
    bool recoveryOnly_ = false;
    size_t scannedRecords_ = 0;
    NSDictionary *active_ = nil;
    [[nodiscard]] bool beginVersion(NSString *, NSDictionary *, NSDictionary *, int);
    [[nodiscard]] bool fail(const char *message);
    [[nodiscard]] bool write(NSDictionary *record);
    [[nodiscard]] NSArray *scan();
public:
    std::string error;
    RegistrationJournal() = default;
    ~RegistrationJournal();
    RegistrationJournal(const RegistrationJournal&) = delete;
    RegistrationJournal& operator=(const RegistrationJournal&) = delete;
    [[nodiscard]] bool open(const std::string& absolutePath, bool readOnly = false, bool existingOnly = false);
    [[nodiscard]] bool requireClear();
    // Existing, read-only, exclusively locked journal only. Never grants recovery.
    [[nodiscard]] NSDictionary *audit();
    [[nodiscard]] NSDictionary *reviewRemoval(NSString *attempt, NSString *expectedHash);
    // Review an unresolved v5 prefix in read-only or existing-only recovery mode;
    // does not append an event. Exact hash and exclusive journal required.
    [[nodiscard]] NSDictionary *reviewLaunchFailure(NSString *attempt, NSString *expectedHash);
    [[nodiscard]] bool recoverLaunchFailure(NSString *attempt,NSString *expectedHash,NSDictionary *evidence);
    [[nodiscard]] bool reconcileNeverLaunched(NSString *attempt,NSString *expectedHash,NSDictionary *evidence);
    [[nodiscard]] static bool validateRecovery(NSDictionary *record, NSDictionary *evidence);
    // Existing-only writable open; exact reviewed hash and fresh owner evidence.
    [[nodiscard]] bool recoverRemoval(NSString *attempt, NSString *expectedHash, NSDictionary *evidence);
    [[nodiscard]] static bool validatePreflight(NSDictionary *identity, NSDictionary *preflight);
    // metadata schema is documented in REGISTRATION_JOURNAL.md.
    [[nodiscard]] bool begin(NSString *attempt, NSDictionary *metadata, NSDictionary *preflight);
    [[nodiscard]] bool beginLaunch(NSString *attempt, NSDictionary *metadata, NSDictionary *preflight);
    [[nodiscard]] bool recordLaunchFailure(NSString *category, NSString *detail, uint64_t observedNS);
    [[nodiscard]] bool append(NSString *kind, NSDictionary *evidence);
    // Bind a launch adapter only to this owner's active, successfully registered
    // v5 prefix. Never authorizes registration or bypasses journal ownership.
    [[nodiscard]] bool matchesRegisteredLaunch(NSString *attempt, NSDictionary *metadata) const;
#ifdef PRO40_JOURNAL_TESTING
    // Fail once at a real persistence boundary; no OS/service operations.
    int failWriteStage = 0;
#endif
};
}
