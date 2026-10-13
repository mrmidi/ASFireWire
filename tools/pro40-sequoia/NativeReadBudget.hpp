// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
class NativeReadBudget {
    unsigned limit_,used_=0;
    uint64_t deadline_=60000000;
    bool cleanup_=false;
    bool rolling_=false,fault_=false;
    uint64_t window_=0,previous_=0;
public:
    explicit NativeReadBudget(unsigned limit):limit_(limit) {}
    static NativeReadBudget sustained() {
        NativeReadBudget result(512);result.rolling_=true;return result;
    }
    bool admit(uint64_t elapsedMicros) {
        if(rolling_ && !cleanup_) {
            if(fault_ || elapsedMicros<previous_)return !(fault_=true);
            previous_=elapsedMicros;
            const auto window=elapsedMicros/1000000;
            if(window!=window_){window_=window;used_=0;}
            if(used_>=limit_)return !(fault_=true);
            ++used_;return true;
        }
        if(elapsedMicros>deadline_ || used_>=limit_)return false;
        ++used_;return true;
    }
    // One independent, bounded teardown allowance. Repeated calls cannot refill.
    void beginCleanup(uint64_t elapsedMicros) {
        if(cleanup_)return;
        cleanup_=true;used_=0;limit_=256;
        deadline_=elapsedMicros>UINT64_MAX-10000000?UINT64_MAX:elapsedMicros+10000000;
    }
};
