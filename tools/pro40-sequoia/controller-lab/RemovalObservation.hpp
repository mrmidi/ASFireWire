// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationJournal.hpp"
#include "JobObservation.hpp"
namespace Pro40Lab {
// macOS pgrep(1): 0 matches, 1 no matches, 2/3 errors. This snapshot is
// neither proof that a helper never ran nor an exit/reap receipt.
inline NSDictionary *removalEvidence(NSDictionary *expected, NSDictionary *candidate,
                                     const JobProcessResult& helper, NSInteger finalStatus) {
    if (!expected || ![candidate[@"identity"] isEqual:expected] ||
        !RegistrationJournal::validatePreflight(expected, candidate) ||
        [candidate[@"raw_status"] integerValue] != 0 || finalStatus != 0 ||
        !helper.usable() || !helper.err.empty()) return nil;
    bool seen = false;
    if (helper.exit == 1 && helper.out.empty()) seen = false;
    else if (helper.exit == 0 && !helper.out.empty()) {
        bool digit = false;
        for (char c : helper.out) {
            if (c >= '0' && c <= '9') digit = true;
            else if (c == '\n' && digit) digit = false;
            else return nil;
        }
        if (digit) return nil; // require complete newline-delimited PID output
        seen = true;
    } else return nil;
    return @{@"not_registered": @YES, @"job_absent": @YES, @"unexpected_helper": @(seen)};
}
}
