// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PlaybackLockCheck.hpp"
namespace Pro40Packets {
// Isolated experiment: after initial acquisition, stop status polling and active
// route reads. Linux dice-stream.c:430-468 starts the domain without a recurring
// status-read loop. This is deliberately REDUCED evidence, not continuous lock
// validation. Packet, generation, notification and teardown checks stay active.
class QuietStartupControl {
    unsigned entered_=0;
public:
    void observeAccepted(const PlaybackLockCheck &check,unsigned progress) noexcept {
        if(!entered_ && check.lockedCount()>=3 && check.validThrough(progress))entered_=progress;
    }
    bool pollingAllowed() const noexcept {return entered_==0;}
    unsigned enteredProgress() const noexcept {return entered_;}
    bool complete(unsigned progress,unsigned budget) const noexcept {
        return entered_ && entered_<budget && progress==budget;
    }
};
}
