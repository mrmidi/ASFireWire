// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <limits>
namespace Pro40Lab {
using LaunchNonce = std::array<uint8_t,16>;
enum class LaunchPhase { initial, prepared, launched, running, exited, removed, complete, blocked };
// These are trusted supervisor observations, NOT claims accepted from a helper.
// No IO, authentication, parser, clock read, journal or cleanup is implemented here.
struct LaunchBaseline {
    bool exactPackage = false, exclusiveJournal = false, dormantRegisteredJob = false;
    bool snapshotCurrent = false, noRunningPID = false;
    uint64_t runs = 0;
};
struct LaunchReady {
    LaunchNonce nonce{};
    bool authenticatedPeer = false, processBirthMatches = false, snapshotCurrent = false;
    int32_t launchdPID = 0, peerPID = 0;
    uint64_t runs = 0;
};
struct LaunchExit {
    bool exactJob = false, snapshotCurrent = false, notRunning = false;
    bool originalInstanceGone = false, exitCodeKnown = false, signalled = false;
    uint64_t runs = 0;
    int32_t code = -1;
};
class VerifierLaunchPolicy final {
    const LaunchNonce nonce_;
    const uint64_t deadline_;
    uint64_t lastTime_ = 0, runs_ = 0;
    LaunchPhase phase_ = LaunchPhase::initial;
    bool cleanExit_ = false;
    bool block() noexcept { phase_ = LaunchPhase::blocked; return false; }
    bool timely(uint64_t now) noexcept {
        if (!deadline_ || now >= deadline_ || now < lastTime_) return block();
        lastTime_ = now; return true;
    }
    bool enter(LaunchPhase expected, uint64_t now) noexcept {
        if (phase_ != expected) return block();
        return timely(now);
    }
public:
    VerifierLaunchPolicy(LaunchNonce nonce, uint64_t deadline) : nonce_(nonce),deadline_(deadline) {}
    VerifierLaunchPolicy(const VerifierLaunchPolicy&) = delete;
    VerifierLaunchPolicy& operator=(const VerifierLaunchPolicy&) = delete;
    LaunchPhase phase() const noexcept { return phase_; }
    bool prepare(LaunchBaseline b, uint64_t now) noexcept {
        if (!enter(LaunchPhase::initial, now)) return false;
        bool nonceValid = false; for (auto byte : nonce_) nonceValid |= byte != 0;
        if (!nonceValid || !b.exactPackage || !b.exclusiveJournal || !b.dormantRegisteredJob ||
            !b.snapshotCurrent || !b.noRunningPID || b.runs == std::numeric_limits<uint64_t>::max()) return block();
        runs_ = b.runs + 1; phase_ = LaunchPhase::prepared; return true;
    }
    bool authorizeLaunch(bool durableIntent, uint64_t now) noexcept {
        if (!enter(LaunchPhase::prepared, now) || !durableIntent) return block();
        phase_ = LaunchPhase::launched; return true;
    }
    bool observeReady(LaunchReady r, uint64_t now) noexcept {
        if (!enter(LaunchPhase::launched, now)) return false;
        if (r.nonce != nonce_ || !r.authenticatedPeer || !r.processBirthMatches || !r.snapshotCurrent ||
            r.launchdPID <= 0 || r.peerPID != r.launchdPID || r.runs != runs_) return block();
        phase_ = LaunchPhase::running; return true;
    }
    bool observeExit(LaunchExit e, uint64_t now) noexcept {
        if (!enter(LaunchPhase::running, now)) return false;
        if (!e.exactJob || !e.snapshotCurrent || !e.notRunning || !e.originalInstanceGone ||
            !e.exitCodeKnown || e.signalled || e.runs != runs_ || e.code < 0 || e.code > 255) return block();
        cleanExit_ = e.code == 0; phase_ = LaunchPhase::exited; return true;
    }
    bool observeRemoval(bool notRegistered, bool exactJobAbsent, bool noUnexpectedHelper, uint64_t now) noexcept {
        if (!enter(LaunchPhase::exited, now)) return false;
        if (!notRegistered || !exactJobAbsent || !noUnexpectedHelper) return block();
        phase_ = LaunchPhase::removed; return true;
    }
    bool recordCompletion(bool durable, uint64_t now) noexcept {
        if (!enter(LaunchPhase::removed, now) || !durable) return block();
        phase_ = LaunchPhase::complete; return true;
    }
    void abandon() noexcept { if (phase_ != LaunchPhase::complete) phase_ = LaunchPhase::blocked; }
    bool confirmedCleanExitAndRemoval() const noexcept { return phase_ == LaunchPhase::complete && cleanExit_; }
    // No identity reply or local endpoint cleanup is proved by this launch policy;
    // admission still belongs to NativeServiceIdentityDecision and its resource owner.
};
}
