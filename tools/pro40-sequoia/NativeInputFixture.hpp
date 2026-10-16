// SPDX-License-Identifier: Apache-2.0
#pragma once
// Test-only receive completion source. No IOFireWire or device access.
#include "SustainedInputFixture.hpp"
#include "NativeSourceQueue.hpp"
#include <cassert>
namespace Pro40Audio::Fixture {
struct NativeSource {
    Source packets;
    NativePackets::SourceQueue queue{7};
    struct Record {std::array<uint8_t,448> bytes{};uint32_t status=0x8411,stamp=0;};
    std::array<Record,64> group{};
    NativeSource() {assert(queue.enableObserver());}
    static void little(uint8_t *p,uint32_t v){for(unsigned n=0;n<4;++n)p[n]=uint8_t(v>>(8*n));}
    void prepare() {
        for(auto &r:group) {
            r={};r.status=0x8411;
            const auto payload=packets.next();const auto cycle=packets.record.arrivalCycle;
            r.stamp=((cycle/8000)<<25)|((cycle%8000)<<12);
            little(r.bytes.data(),r.stamp>>12);
            little(r.bytes.data()+4,uint32_t(payload.size())<<16 | 1u<<14 | 4u<<8 | 0xa0);
            std::copy(payload.begin(),payload.end(),r.bytes.begin()+8);
        }
    }
    bool append() {
        const auto result=queue.append(packets.ordinal-64,7,[&](uint64_t n) {
            const auto &r=group[n%64];return NativePackets::CompletedBuffer{r.bytes,r.status,r.stamp};
        });
        // Immediately reuse the synthetic DMA storage after the owned copy.
        for(auto &r:group)r.bytes.fill(0xee);
        return result;
    }
};
}
