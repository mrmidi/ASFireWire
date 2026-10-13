// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeServiceIdentityDecision.hpp"
#include "NativeSessionRunner.hpp"
#include <utility>

namespace Pro40Audio {
enum class ServiceEntryStatus { entered, denied, stopped, consumed };
struct ServiceEntryResult {
    ServiceEntryStatus status;
    // No fabricated runner outcome when the continuation was never called.
    std::optional<Pro40::NativeSessionResult> session;
};
// One control-thread owner. Decision and stop context outlive enter(). The
// controller retains all pre-session service resources, including on refusal.
// This gate neither cleans them up nor establishes trust. Its continuation owns
// device lease acquisition and runner invocation; keep IO outside this class.
class NativeServiceEntryGate {
    const NativeServiceIdentityDecision &decision_;
    bool consumed_=false;
public:
    explicit NativeServiceEntryGate(const NativeServiceIdentityDecision &decision)noexcept:decision_(decision){}
    NativeServiceEntryGate(const NativeServiceEntryGate &)=delete;
    NativeServiceEntryGate &operator=(const NativeServiceEntryGate &)=delete;
    [[nodiscard]] ServiceEntryStatus admit(Pro40::StopQuery stop) {
        if(consumed_)return ServiceEntryStatus::consumed;
        consumed_=true;
        if(!decision_.permitsAdmission())return ServiceEntryStatus::denied;
        if(stop.requested())return ServiceEntryStatus::stopped;
        return ServiceEntryStatus::entered;
    }
    template<class Continue>
    [[nodiscard]] ServiceEntryResult enter(Pro40::StopQuery stop,Continue &&continuation) {
        const auto status=admit(stop);
        if(status!=ServiceEntryStatus::entered)return {status,std::nullopt};
        // Runner must still observe stops arriving after this check.
        return {ServiceEntryStatus::entered,std::forward<Continue>(continuation)()};
    }
};
}
