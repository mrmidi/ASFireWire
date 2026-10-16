// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

// DICE ENABLE only: Linux dice-transaction.c:96-129, dice-interface.h:125.
// Caller must have prepared all streams and enforce its explicit startup policy.
// Ordinary runner arms first; isolated enable-first diagnostic arms after enable.
// Cleanup deliberately checks ownership/generation, not clock lock: a lost
// clock must not prevent disabling the device. Never restore ENABLE=1.
template<class Backend> class Pro40StreamEnable {
    Backend &io_;
    bool attempted_ = false, cleared_ = false;
public:
    explicit Pro40StreamEnable(Backend &io):io_(io) {}
    Pro40StreamEnable(const Pro40StreamEnable &) = delete;
    Pro40StreamEnable &operator=(const Pro40StreamEnable &) = delete;
    ~Pro40StreamEnable() { (void)clear(); }
    int32_t enable() {
        if (attempted_ || cleared_) return -1;
        auto status = io_.enableGuard();
        if (status) return status;
        attempted_ = true; // A failed ACK may still have applied the write.
        status = io_.writeEnabled(1);
        uint32_t value = 0;
        if (!status) status = io_.readEnabled(value);
        return status ? status : value == 1 ? 0 : -2;
    }
    int32_t clear() {
        if (cleared_) return 0;
        cleared_ = true;
        if (!attempted_) return 0;
        auto status = io_.ownerGuard();
        if (!status) status = io_.writeEnabled(0);
        uint32_t value = 1;
        if (!status) status = io_.readEnabled(value);
        return status ? status : value == 0 ? 0 : -2;
    }
};
