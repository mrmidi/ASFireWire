// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalDuplexDriverState.hpp"
#include <chrono>
#include <thread>
namespace Pro40Audio {
// One non-realtime owner per driver. Source loss never consumes host IO holds
// or changes the published device state on an audio/connection callback. SDK
// AudioServerPlugIn.h:65-80 requires Request -> host StopIO -> Perform for an
// IO-affecting change. Cleanup does not depend on the host accepting that request.
class HalSourceLossControl final {
public:
    static constexpr uint64_t action=0x5034304c4f535301ULL;
    using Notify=OSStatus (*)(void *,uint64_t);
    enum class Phase {idle,pending,performed,aborted,rejected};
private:
    HalDuplexDriverState &state_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<Phase> phase_{Phase::idle};
    void run(Notify notify,void *context) {
        while(!stop_.load(std::memory_order_acquire)) {
            if(state_.retireEnded()) {
                phase_.store(Phase::pending,std::memory_order_release);
                // No state, cleanup or telemetry lock crosses this call. The
                // host may synchronously reenter StopIO, getters and Perform.
                const auto status=notify(context,action);
                if(status!=noErr) {
                    auto pending=Phase::pending;
                    (void)phase_.compare_exchange_strong(pending,Phase::rejected);
                    os_log_error(OS_LOG_DEFAULT,"Pro40Lab source loss configuration request failed: status=%{public}d",int(status));
                }
            }
            if(phase_.load(std::memory_order_acquire)!=Phase::idle) {
                const auto result=state_.drainSourceLoss();
                if(result!=HalDuplexDriverState::LossDrain::pending) {
                    os_log_error(OS_LOG_DEFAULT,"Pro40Lab source loss joined: cleanup=%{public}u",unsigned(result==HalDuplexDriverState::LossDrain::clean));
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
public:
    explicit HalSourceLossControl(HalDuplexDriverState &state):state_(state){}
    ~HalSourceLossControl(){stopAndJoin();}
    bool start(Notify notify,void *context) {
        if(!notify || thread_.joinable() || stop_.load())return false;
        try {thread_=std::thread([=,this]{run(notify,context);});}catch(...){return false;}
        return true;
    }
    // Final Release must quiesce external users. Join before destroying state,
    // the host pointer, callback recorder, or any other referenced driver member.
    void stopAndJoin() {
        stop_.store(true,std::memory_order_release);
        if(thread_.joinable())thread_.join();
    }
    bool alive() const noexcept {return phase_.load(std::memory_order_acquire)!=Phase::performed;}
    OSStatus perform(uint64_t requested,void *info) noexcept {
        if(requested!=action || info)return kAudioHardwareUnsupportedOperationError;
        HalSilenceModel::Snapshot snapshot;
        if(!state_.read(snapshot) || snapshot.running())return kAudioHardwareIllegalOperationError;
        auto pending=Phase::pending;
        if(!phase_.compare_exchange_strong(pending,Phase::performed))return kAudioHardwareIllegalOperationError;
        os_log_error(OS_LOG_DEFAULT,"Pro40Lab source loss configuration performed: alive=0");return noErr;
    }
    OSStatus abort(uint64_t requested,void *info) noexcept {
        if(requested!=action || info)return kAudioHardwareUnsupportedOperationError;
        auto pending=Phase::pending;
        if(!phase_.compare_exchange_strong(pending,Phase::aborted))return kAudioHardwareIllegalOperationError;
        os_log_error(OS_LOG_DEFAULT,"Pro40Lab source loss configuration aborted; audio remains blocked");
        return noErr;
    }
};
}
