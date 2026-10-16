// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeCompletionLayout.hpp"
#include <cstdint>
namespace NativePackets {
// Validate the completed prefix of an intentionally stopped two-bank program.
// Packet semantics remain in the existing progress/replay validators. This
// checks transport accounting after native Stop and the finalization callback.
struct StoppedPrefix {
    uint64_t progress=0,callbacks=0,nativeCallbacks=0,budget=0;
    CompletionLayout layout{};
    unsigned handoffs=0;
    bool fault=false;
    unsigned bank=128;
    bool valid() const {
        const auto notifications=layout.notifications(progress);
        if(fault || (bank!=128 && bank!=256) || (layout.bank && layout.bank!=bank) ||
           progress<512 || progress>=budget || progress!=callbacks ||
           !notifications || nativeCallbacks!=*notifications)
            return false;
        const auto banks=progress/bank;
        return handoffs>=banks-2 && handoffs<=banks;
    }
    unsigned completions(unsigned slot) const {
        if(bank!=128 && bank!=256)return 0;
        const auto slots=2*bank;
        return slot>=slots || progress<=slot?0:unsigned((progress-1-slot)/slots+1);
    }
};
inline bool stoppedSourceQueueValid(uint64_t first,uint64_t next,uint64_t copied,
                                    uint64_t progress,unsigned queued,bool fault,unsigned sourceBatch=128) {
    return !fault && (sourceBatch==128 || sourceBatch==256) && first<=next && first%sourceBatch==0 && next==progress &&
        copied==first && (next-first)%64==0 && queued==(next-first)/64 && queued<=16;
}
}
