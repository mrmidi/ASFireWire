// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace Pro40Lab {
// Cleanup permits resource retirement. Only a clean, healthy stream permits
// launchd to prepare another session automatically; a fault must stay visible.
struct DelegatedSessionOutcome {
    bool joined=false,receiptSent=false,streamPassed=false,nativeObserved=false;
    bool nativeEvidenceClean=false,nativeCleanupClean=false,workerCollected=false;
    constexpr bool restartPermitted()const {
        return joined && receiptSent && streamPassed && nativeObserved &&
            nativeEvidenceClean && nativeCleanupClean && workerCollected;
    }
};
}
