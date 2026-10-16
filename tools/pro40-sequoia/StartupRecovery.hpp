// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40 {
// Native adaptation of ASFireWire PR180 / 1ee5be85, SessionScheduler.cpp:267-306.
// Behavior only: remember idle RX/TX_CFG_CHG; restart once after a successful
// start and a quiet period. The caller owns transport stop/finalization.
// DICE event bits: references/linux-dice/dice-interface.h:66-71.
class StartupRecovery {
    bool pending_=false,started_=false,consumed_=false;
    uint64_t startedAt_=0,quietSince_=0;
public:
    static constexpr uint64_t quietMicros=400000,limitMicros=2000000;
    enum class Decision {wait,restart,timeout};
    void noteIdle(uint32_t bits) {if(!consumed_ && (bits&3))pending_=true;}
    bool pending() const {return pending_ && !consumed_;}
    void started(uint64_t now) {
        if(pending() && !started_){started_=true;startedAt_=quietSince_=now;}
    }
    Decision poll(uint64_t now,uint64_t lastConfig) {
        if(!pending() || !started_)return Decision::wait;
        if(lastConfig>quietSince_)quietSince_=lastConfig;
        if(now<startedAt_ || now<quietSince_)return Decision::timeout;
        if(now-startedAt_>=limitMicros)return Decision::timeout;
        if(now-quietSince_>=quietMicros){consumed_=true;return Decision::restart;}
        return Decision::wait;
    }
};
}
