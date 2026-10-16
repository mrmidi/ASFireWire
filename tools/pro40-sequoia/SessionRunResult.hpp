// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SessionLifecycle.hpp"
namespace Pro40 {
struct NativeSessionResult {
    int exitCode=1;
    std::optional<SessionOutcome> outcome;
    bool accepted() const {return exitCode==0 && outcome && outcome->accepted();}
};
// Combine post-close errors without erasing an earlier lifecycle error. A
// missing closed outcome can never become an accepted result.
inline NativeSessionResult closedSessionResult(std::optional<SessionOutcome> outcome,
                                               int32_t transfer,int32_t evidence,int32_t cleanup) {
    if(outcome) {
        if(!outcome->transfer)outcome->transfer=transfer;
        if(!outcome->evidence)outcome->evidence=evidence;
        if(!outcome->cleanup)outcome->cleanup=cleanup;
    }
    const bool accepted=outcome && outcome->accepted();
    return {accepted?0:2,outcome};
}
}
