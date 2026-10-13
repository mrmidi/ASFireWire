// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeIdentityOutcome.hpp"
#include "NativePeerCleanup.hpp"
namespace Pro40Audio {
// One immutable decision and retained outcome, not a new session lifecycle.
// Requires a result from the trusted supervisor; does not establish helper trust,
// revalidate a peer or authorize PCM. No automatic retry/recovery/reopening.
class NativeIdentityDecision {
public:
    enum class State {verified,rejected,cleanupBlocked};
private:
    IdentityLabOutcome outcome_;
    const State state_;
    static State decide(const IdentityLabOutcome &outcome)noexcept{
        if(!outcome.reaped || outcome.pendingPID || outcome.resourceError)return State::cleanupBlocked;
        if(outcome.process){
            const auto &process=*outcome.process;
            if(process.state()!=NativeChildProcess::State::reaped || process.waitError() || process.signalError())
                return State::cleanupBlocked;
        } else if(outcome.status || outcome.reason=="verified")return State::cleanupBlocked;
        if(outcome.status && *outcome.status==0 && outcome.reason=="verified" && !outcome.preparationError && outcome.process){
            const int status=outcome.process->exitStatus();
            if(WIFEXITED(status) && WEXITSTATUS(status)==0)return State::verified;
            return State::cleanupBlocked; // Contradictory success evidence.
        }
        return State::rejected;
    }
public:
    explicit NativeIdentityDecision(IdentityLabOutcome outcome)noexcept:
        outcome_(std::move(outcome)),state_(decide(outcome_)){}
    NativeIdentityDecision(const NativeIdentityDecision &)=delete;
    NativeIdentityDecision &operator=(const NativeIdentityDecision &)=delete;
    State state()const noexcept{return state_;}
    bool permitsAdmission()const noexcept{return state_==State::verified;}
    bool helperCleanupConfirmed()const noexcept{return state_!=State::cleanupBlocked;}
    const IdentityLabOutcome &outcome()const noexcept{return outcome_;}
    // Explicit supervisor recovery updates diagnostics only. The admission and
    // cleanup decision stays latched even if a child finally exits later.
    NativeChildProcess::State observeRetainedChild(bool requestStop=false)noexcept{
        if(!outcome_.process)return NativeChildProcess::State::empty;
        auto &child=*outcome_.process;
        if(requestStop)(void)child.requestStop();
        const auto state=child.poll();
        outcome_.reaped=state==NativeChildProcess::State::reaped;
        outcome_.pendingPID=outcome_.reaped?0:child.pid();
        return state;
    }
    // Other local joins/port closes are independently required. This component
    // may downgrade their result, never promote incomplete cleanup to success.
    LocalCleanup combineLocalCleanup(LocalCleanup other)const noexcept{
        return helperCleanupConfirmed()?other:LocalCleanup::incomplete;
    }
};
}
