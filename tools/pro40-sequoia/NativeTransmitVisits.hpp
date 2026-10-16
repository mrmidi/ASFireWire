// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include "NativeTransmitProgress.hpp"
namespace NativePackets {
class TransmitVisits {
public:
    struct Record {uint32_t status=0,timestamp=0;};
private:
    std::array<Record,8192> records_{};
    TransmitProgress progress_;
public:
    explicit TransmitVisits(uint32_t start,unsigned budget=384,unsigned slots=256):
        progress_(start,(budget==384 || budget==512 || budget==640 || budget==1024 || budget==4096 || budget==8192)?budget:0,slots) {}
    bool fault() const {return progress_.fault();}
    unsigned next() const {return unsigned(progress_.next());}
    bool accept(unsigned physical,uint32_t status,uint32_t stamp) {
        const auto n=next();
        if(!progress_.accept(physical,status,stamp))return false;
        records_[n]={status,stamp};return true;
    }
    Record view(unsigned n) const {return n<next()?records_[n]:Record{};}
};
}
