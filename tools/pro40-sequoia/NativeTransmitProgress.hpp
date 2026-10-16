// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace NativePackets {
// Hardware can report the bank's last completion while its stop lead is still
// the bank size. Wait one additional completion on BOTH ports; native upper
// bank bound and64/32-cycle lead guards remain authoritative. Bounded ordinals.
constexpr bool transmitBankReady(unsigned ordinal,unsigned first,unsigned second,unsigned bank=128) {
    const uint64_t required=(uint64_t(ordinal)+1)*bank+1;
    return first>=required && second>=required;
}
constexpr bool transmitBankPending(unsigned ordinal,unsigned first,unsigned second,unsigned bank=128) {
    const uint64_t boundary=(uint64_t(ordinal)+1)*bank;
    return first>=boundary && second>=boundary;
}
// Runtime completion state, independent of optional observation storage.
// Preserve the validated native TX cycle comparison modulo 32000 cycles.
class TransmitProgress {
    uint64_t next_=0,budget_;
    uint32_t start_,slots_;
    bool fault_;
public:
    explicit TransmitProgress(uint32_t start,uint64_t budget,unsigned slots=256):budget_(budget),
        start_(((start>>25)&3)*8000+((start>>12)&8191)),
        slots_(slots),fault_(!budget || (slots!=256 && slots!=512) || (start&0xf0000fff) || ((start>>12)&8191)>=8000) {}
    bool fault() const {return fault_;}
    uint64_t next() const {return next_;}
    bool accept(unsigned physical,uint32_t status,uint32_t stamp) {
        if(fault_)return false;
        const auto cycle=((stamp>>25)&3)*8000+((stamp>>12)&8191);
        if(next_==budget_ || physical!=next_%slots_ || (status&31)!=17 ||
            (stamp&0xf0000fff) || ((stamp>>12)&8191)>=8000 ||
            cycle!=(start_+next_%32000)%32000) {
            fault_=true;return false;
        }
        ++next_;return true;
    }
};
static_assert(sizeof(TransmitProgress)<=32);
}
