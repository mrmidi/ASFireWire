// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexProvisionRegistry.hpp"
#include <chrono>
namespace Pro40Audio {
// Control-thread only. Retry solely a code-authenticated ABSENT response while
// launchd replaces a successfully exited controller. Identity/protocol/transport
// errors remain terminal. No worker has been requested or released at this point.
template<class Query,class Now,class Wait>
DuplexProvisionRegistry::Lookup awaitProvisionController(Query query,Now now,Wait wait,
                                                        std::chrono::milliseconds budget) {
    using Result=DuplexProvisionRegistry::Result;
    if(budget.count()<=0 || budget>std::chrono::seconds(5))return {Result::invalid,{}};
    const auto began=now();auto last=began;
    for(;;) {
        const auto current=now();
        if(current<last)return {Result::transport,{}};
        last=current;
        if(current-began>=budget)return {Result::absent,{}};
        auto result=query();
        const auto ended=now();
        if(ended<last)return {Result::transport,{}};
        last=ended;
        if(ended-began>=budget)return {Result::absent,{}};
        if(result.result!=Result::absent)return result;
        wait();
    }
}
}
