// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ControlInputPeak.hpp"
#include <array>
#include <cstdio>
namespace Pro40 {
// Control-thread snapshots only, never in a DMA/refill callback or scheduled
// startup interval. Peak reads may consume accumulated indicators; one nonzero
// snapshot is not proof of sustained recovery. See ControlInputPeak references.
class InputBoundaryTrace {
public:
    enum class Stage {beforeClock,afterClock,afterDisable,channelsRestored,ownerReleased};
    struct Record {Stage stage{};uint64_t begin=0,end=0;ControlInputPeak peak{};};
private:
    std::array<Record,5> records_{};
    unsigned used_=0,rejected_=0;
public:
    template<class Read,class Guard,class Now>
    void capture(Stage stage,Read read,Guard guard,Now now){
        if(used_==records_.size() || (used_&&unsigned(stage)<=unsigned(records_[used_-1].stage))){++rejected_;return;}
        auto &r=records_[used_++];r.stage=stage;r.begin=now();
        r.peak.capture(read,guard);r.end=now();
    }
    unsigned size()const{return used_;}
    unsigned rejected()const{return rejected_;}
    const Record &operator[](unsigned n)const{return records_[n];}
    void dump(FILE *log)const{
        std::fprintf(log,"input-boundary-trace schema=1 retained=%u rejected=%u\n",used_,rejected_);
        for(unsigned i=0;i<used_;++i){const auto &r=records_[i];
            std::fprintf(log,"input-boundary stage=%u begin-us=%llu end-us=%llu valid=%u io=%08x error=%u raw=%08x peak=%u\n",
                unsigned(r.stage),static_cast<unsigned long long>(r.begin),static_cast<unsigned long long>(r.end),
                unsigned(r.peak.valid),unsigned(r.peak.ioStatus),r.peak.error,r.peak.raw,r.peak.peak);
        }
    }
};
}
