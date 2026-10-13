// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40 {
// A stop never authorizes Start/ENABLE, even if the timing window is valid.
// Existing transport/ownership failures take precedence and skip this policy.
enum class StartupDecision {proceed, interrupted, timedOut};
constexpr StartupDecision startupDecision(bool stop,uint32_t lead,uint32_t minimum,
                                         uint32_t maximum,bool expired=false) {
    if(stop)return StartupDecision::interrupted;
    return expired || lead<minimum || lead>maximum ? StartupDecision::timedOut : StartupDecision::proceed;
}
}
