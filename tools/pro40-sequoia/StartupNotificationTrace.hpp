// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace Pro40 {
// Single run-loop owner only. Times describe host callback delivery, not device
// emission. Fixed storage and no IO until dump() after dispatcher removal.
class StartupNotificationTrace {
public:
    enum class Phase { ownership, clock, preparation, enable, streaming, cleanup };
    enum class Kind { phase, notification, invalid, skipped, clockWriteStart, clockWriteEnd, clockAccepted, existingClockCheck, existingClockVerified };
    struct Record { uint64_t us; Phase phase; Kind kind; uint32_t value; };
    static constexpr size_t capacity=64;
private:
    std::array<Record,capacity> records_{};
    size_t used_=0;
    uint32_t dropped_=0;
    Phase phase_=Phase::ownership;
    bool enabled_=false;
    std::chrono::steady_clock::time_point origin_{};
public:
    void start() {
        if(enabled_)return;
        origin_=std::chrono::steady_clock::now();enabled_=true;
        record(Kind::phase);
    }
    uint64_t now() const {
        return enabled_?uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-origin_).count()):0;
    }
    void at(uint64_t us,Kind kind,uint32_t value=0) {
        if(!enabled_)return;
        if(used_==capacity){if(dropped_!=std::numeric_limits<uint32_t>::max())++dropped_;return;}
        records_[used_++]={us,phase_,kind,value};
    }
    void record(Kind kind,uint32_t value=0){at(now(),kind,value);}
    void phase(Phase next){phase_=next;record(Kind::phase);}
    size_t size() const{return used_;}
    uint32_t dropped() const{return dropped_;}
    const Record &operator[](size_t i) const{return records_[i];}
    void dump(FILE *log) const {
        if(!enabled_)return;
        constexpr const char *phases[]={"ownership","clock","preparation","enable","streaming","cleanup"};
        constexpr const char *kinds[]={"phase","notification","invalid","skipped","clock-write-start","clock-write-end","clock-accepted","existing-clock-check","existing-clock-verified"};
        std::fprintf(log,"startup-trace schema=1 retained=%zu capacity=%zu dropped=%u time=host-delivery-us\n",used_,capacity,dropped_);
        for(size_t i=0;i<used_;++i){const auto &r=records_[i];
            std::fprintf(log,"startup-trace-event index=%zu us=%llu phase=%s kind=%s value=%08x\n",i,
                static_cast<unsigned long long>(r.us),phases[unsigned(r.phase)],kinds[unsigned(r.kind)],r.value);
        }
    }
};
}
