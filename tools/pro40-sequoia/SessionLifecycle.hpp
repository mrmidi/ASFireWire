// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <optional>
namespace Pro40 {
enum class SessionPhase {idle,starting,running,stopping,closed};
enum class StopReason {completed,interrupted,error,watchdog,invalidNotification};
constexpr const char *name(StopReason reason) {
    switch(reason) {
    case StopReason::completed:return "completed";
    case StopReason::interrupted:return "interrupted";
    case StopReason::watchdog:return "watchdog";
    case StopReason::invalidNotification:return "invalid-notification";
    default:return "error";
    }
}
struct SessionOutcome {
    StopReason reason=StopReason::error;
    int32_t transfer=0,evidence=0,cleanup=0;
    bool accepted() const {return reason==StopReason::completed && !transfer && !evidence && !cleanup;}
};
// Single-owner control state; owns no native pointers and performs no IO.
// Caller must actually quiesce/release native resources before finish(). This
// contract is not a replacement for DMA finalization or mapping ownership.
class SessionLifecycle {
    SessionPhase phase_=SessionPhase::idle;
    bool stopRequested_=false;
    SessionOutcome result_{};
public:
    SessionPhase phase() const {return phase_;}
    bool stopRequested() const {return stopRequested_;}
    bool start() {
        if(phase_!=SessionPhase::idle && (phase_!=SessionPhase::closed || result_.cleanup))return false;
        result_={};stopRequested_=false;phase_=SessionPhase::starting;return true;
    }
    bool requestStop() {
        if(phase_!=SessionPhase::starting && phase_!=SessionPhase::running)return false;
        stopRequested_=true;return true;
    }
    bool running() {
        if(phase_!=SessionPhase::starting || stopRequested_)return false;
        phase_=SessionPhase::running;return true;
    }
    bool beginStop(StopReason reason,int32_t transfer) {
        if(phase_!=SessionPhase::starting && phase_!=SessionPhase::running)return false;
        if(reason==StopReason::completed && (phase_!=SessionPhase::running || stopRequested_ || transfer))return false;
        result_.reason=reason;result_.transfer=transfer;phase_=SessionPhase::stopping;return true;
    }
    bool finish(int32_t evidence,int32_t cleanup) {
        if(phase_!=SessionPhase::stopping)return false;
        result_.evidence=evidence;result_.cleanup=cleanup;phase_=SessionPhase::closed;return true;
    }
    std::optional<SessionOutcome> outcome() const {
        return phase_==SessionPhase::closed?std::optional<SessionOutcome>(result_):std::nullopt;
    }
};
}
