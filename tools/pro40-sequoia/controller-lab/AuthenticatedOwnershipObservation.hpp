// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "JobObservation.hpp"
#include "OwnershipQuery.hpp"
#include "OwnershipQueryClientPins.hpp" // Trusted generated package policy.
namespace Pro40Lab {
inline JobProcessResult authenticatedOwnershipObservation() {
    SecRequirementRef raw=nullptr;JobProcessResult result;
    if(SecRequirementCreateWithString(CFSTR(PRO40_OWNERSHIP_SERVER_REQUIREMENT),0,&raw)!=errSecSuccess) {
        result.ioError=true;result.err="ownership service requirement unavailable";return result;
    }
    Pro40Audio::IdentityDetail::Owned<SecRequirementRef> requirement(raw);
    auto answer=OwnershipQuery::query(PRO40_OWNERSHIP_SERVICE,requirement.get());
    if(!answer.ok()) {
        result.ioError=true;result.err="authenticated ownership query failed: transport="+std::to_string(answer.transport)+
            " status="+std::to_string(unsigned(answer.status));return result;
    }
    // This is the authenticated service's successfully exited/reaped fixed
    // Apple child, not a child spawned by the caller. The service returns OK
    // only after that lifecycle proof and scope validation. No cached snapshot
    // or interactive fallback. The surrounding collector measures fresh IPC
    // chronology and still obtains/validates the launchd job locally.
    result.reaped=true;result.exit=0;result.out=std::move(answer.text);return result;
}
}
