// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeProcessBirth.hpp"
#include "RegistrationRecoveryOwner.hpp"
#include <cerrno>
#include <climits>
namespace Pro40Lab {
// Only called with the journal's validated identity. No signaling and no fallback
// from a generic read failure to "absent".
inline OriginalControllerState probeOriginalController(NSDictionary *identity) {
    const long long rawPID = [identity[@"pid"] longLongValue];
    if (rawPID <= 0 || rawPID > INT_MAX) return OriginalControllerState::unknown;
    const int pid = static_cast<int>(rawPID);
    if (auto birth = NativeProcessBirth::read(pid)) {
        NSString *boot = [NSString stringWithUTF8String:birth->boot];
        bool same = [boot caseInsensitiveCompare:identity[@"boot"]] == NSOrderedSame &&
            birth->seconds == [identity[@"start_seconds"] unsignedLongLongValue] &&
            birth->microseconds == [identity[@"start_microseconds"] unsignedLongLongValue];
        return same ? OriginalControllerState::sameInstance : OriginalControllerState::differentInstance;
    }
    // Confirm a missing PID with the actual libproc error. EPERM, partial data,
    // unavailable boot identity and other failures stay unknown.
    proc_bsdinfo info{};
    errno = 0;
    int count = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    return count == 0 && errno == ESRCH ? OriginalControllerState::absent : OriginalControllerState::unknown;
}
}
