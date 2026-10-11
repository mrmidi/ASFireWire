// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once
#include "../AMDTP/AmdtpRateGeometry.hpp"
#include "MotuBlockLayout.hpp"
#include "MotuSph.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>

namespace ASFW::Encoding::Motu {
enum class MotuSphReferenceCause : uint8_t { kNone, kBootstrap, kRxRebind };

// TX-owned clock. Raw RX SPH/frame differences supply the period; a fresh
// device stamp, projected by audio-frame index, supplies the absolute error.
// FFADO uses its receive sync source's ticks/frame (MotuTransmitStreamProcessor.cpp:299).
// The 13-frame target is measured in the paired 828mk3 48k observer captures;
// applying it to other MOTU models/rates is the common best-effort policy.
class SphSynthesizer final {
  public:
    static constexpr int64_t kOneQ32 = int64_t{1} << 32;
    struct Observation {
        uint64_t generation{0}, updates{0}, frames{0};
        int64_t ticks{0};
        uint64_t rxFirstFrame{0}, txFirstFrame{0};
        uint32_t sphTicks{0}, replayEpoch{0};
    };
    struct State {
        bool configured{false}, seeded{false}, locked{false}, outputMuted{false};
        bool feedbackUpdated{false}, phaseReferenceReset{false}, stepClamped{false};
        bool hardResyncRequired{false}, phaseRepairApplied{false};
        int64_t stepQ32{0}, nominalStepQ32{0}, measuredStepQ32{0};
        int64_t phaseErrorQ32{0}, phaseErrorTicks{0};
        uint64_t streamGeneration{0}, rxFrames{0};
        int64_t rxTicks{0}, txCorrectionQ32{0};
        uint64_t decisions{0}, hardResyncRequests{0}, phaseRepairs{0};
        uint64_t muteTransitions{0}, unmuteTransitions{0};
        uint32_t observationDrops{0}, dropBridgeNotAdvanced{0};
        MotuSphReferenceCause referenceCause{MotuSphReferenceCause::kNone};
    };
    [[nodiscard]] static constexpr int64_t NominalStepQ32(uint32_t rate) noexcept {
        return rate ? static_cast<int64_t>((uint64_t{kTicksPerSecond} << 32) / rate) : 0;
    }
    [[nodiscard]] bool Configure(uint32_t rate, uint32_t offsetFrames = 13) noexcept {
        rate_ = RateToIndex(rate) >= 0 && offsetFrames < rate / 2 ? rate : 0;
        offsetFrames_ = offsetFrames;
        const uint64_t packetFrames =
            rate_ ? AmdtpRateGeometryForSampleRate(rate_)->sytIntervalFrames : 0;
        interval_ =
            packetFrames
                ? ((uint64_t{512} * rate_ + 24000 * packetFrames) / (48000 * packetFrames)) *
                      packetFrames
                : 0;
        Reset();
        return state_.configured;
    }
    void Reset() noexcept {
        state_ = {};
        state_.configured = rate_ != 0;
        state_.outputMuted = state_.configured;
        state_.muteTransitions = state_.configured ? 1 : 0;
        state_.stepQ32 = state_.nominalStepQ32 = state_.measuredStepQ32 = NominalStepQ32(rate_);
        phaseQ32_ = previousTicks_ = 0;
        previousFrames_ = lastUpdates_ = 0;
        replayEpoch_ = 0;
        haveReference_ = hardLatched_ = false;
    }
    // Called with the control block's live generation before seed/observation.
    void BeginGeneration(uint64_t generation) noexcept {
        if (!generation || generation == state_.streamGeneration)
            return;
        if (state_.streamGeneration) {
            const uint64_t mutes = state_.muteTransitions + !state_.outputMuted;
            const uint64_t unmutes = state_.unmuteTransitions;
            Reset();
            state_.muteTransitions = mutes;
            state_.unmuteTransitions = unmutes;
        }
        state_.streamGeneration = generation;
    }
    [[nodiscard]] State Current() const noexcept { return state_; }
    [[nodiscard]] uint64_t DecisionIntervalFrames() const noexcept { return interval_; }
    [[nodiscard]] uint32_t PresentationOffsetFrames() const noexcept { return offsetFrames_; }
    [[nodiscard]] bool SeedOnce(int64_t ticks) noexcept {
        if (!state_.configured || state_.seeded)
            return false;
        phaseQ32_ = int64_t{WrapTick(ticks)} * kOneQ32;
        state_.seeded = true;
        return true;
    }
    [[nodiscard]] uint32_t NextSph() noexcept {
        if (!state_.seeded)
            return 0;
        // Round the wire stamp; retain the fractional accumulator itself.
        const uint32_t tick = WrapTick((phaseQ32_ + kOneQ32 / 2) / kOneQ32);
        phaseQ32_ = Normalize(static_cast<__int128>(phaseQ32_) + state_.stepQ32);
        state_.txCorrectionQ32 = Saturate(static_cast<__int128>(state_.txCorrectionQ32) +
                                          state_.stepQ32 - state_.nominalStepQ32);
        return SphFromTick(tick);
    }
    [[nodiscard]] bool Observe(const Observation& in) noexcept {
        if (!state_.configured || !state_.seeded || !in.generation || !in.updates || !in.frames ||
            in.ticks <= 0 || !in.replayEpoch || in.sphTicks >= kTicksPerSecond ||
            in.generation != state_.streamGeneration)
            return false;
        const __int128 frameDelta = static_cast<__int128>(in.txFirstFrame) - in.rxFirstFrame;
        // The SPH has a one-second domain. Refuse a stale/unaligned frame
        // projection outside half a second rather than folding it into a lock.
        if (frameDelta > rate_ / 2 || frameDelta < -static_cast<int64_t>(rate_ / 2)) {
            ++state_.observationDrops;
            return false;
        }
        if (haveReference_ && (in.updates <= lastUpdates_ || in.frames <= state_.rxFrames ||
                               in.ticks <= state_.rxTicks)) {
            ++state_.observationDrops;
            state_.dropBridgeNotAdvanced += in.updates == lastUpdates_;
            return false;
        }
        lastUpdates_ = in.updates;
        state_.rxFrames = in.frames;
        state_.rxTicks = in.ticks;
        const bool rebind = haveReference_ && in.replayEpoch != replayEpoch_;
        if (haveReference_ && !rebind && in.frames - previousFrames_ < interval_)
            return false;
        state_.feedbackUpdated = state_.phaseReferenceReset = state_.stepClamped = false;
        state_.hardResyncRequired = state_.phaseRepairApplied = false;
        state_.referenceCause = MotuSphReferenceCause::kNone;
        ++state_.decisions;
        // An external execution-time seed is not an RX rate observation. The
        // first observation establishes only the rate reference, never phase.
        if (!haveReference_ || rebind) {
            previousFrames_ = in.frames;
            previousTicks_ = in.ticks;
            replayEpoch_ = in.replayEpoch;
            haveReference_ = true;
            hardLatched_ = false;
            state_.phaseReferenceReset = true;
            state_.referenceCause =
                rebind ? MotuSphReferenceCause::kRxRebind : MotuSphReferenceCause::kBootstrap;
            if (rebind) {
                state_.muteTransitions += !state_.outputMuted;
                state_.locked = false;
                state_.outputMuted = true;
            }
            state_.stepQ32 = state_.measuredStepQ32 = state_.nominalStepQ32;
            return true;
        }
        const uint64_t elapsed = in.frames - previousFrames_;
        const __int128 measured =
            (static_cast<__int128>(in.ticks - previousTicks_) * kOneQ32) / elapsed;
        // 50160 Hz upper tracking rate is captured from the official driver;
        // 41895 Hz lower tracking rate is PR191 policy, not a measurement.
        const auto limit = [this](uint32_t trackingRate) {
            return static_cast<int64_t>((static_cast<__int128>(kTicksPerSecond) * 48000 * kOneQ32) /
                                        (static_cast<__int128>(rate_) * trackingRate));
        };
        const int64_t period = std::clamp(Saturate(measured), limit(50160), limit(41895));
        const __int128 target =
            static_cast<__int128>(in.sphTicks) * kOneQ32 + (frameDelta + offsetFrames_) * period;
        const int64_t error = SignedDifference(target - phaseQ32_);
        const bool hard = error > int64_t{4 * kTicksPerCycle} * kOneQ32 ||
                          error < -int64_t{4 * kTicksPerCycle} * kOneQ32;
        const __int128 requested =
            measured +
            (hard ? 0 : error / (static_cast<__int128>(elapsed) * (state_.locked ? 4 : 1)));
        state_.stepQ32 = std::clamp(Saturate(requested), limit(50160), limit(41895));
        state_.stepClamped = state_.stepQ32 != requested;
        state_.measuredStepQ32 = Saturate(measured);
        state_.phaseErrorQ32 = error;
        state_.phaseErrorTicks = error / kOneQ32;
        state_.feedbackUpdated = true;
        state_.hardResyncRequired = hard;
        const bool wasMuted = state_.outputMuted;
        if (hard)
            state_.locked = false;
        else if (error < (6144 * kOneQ32) / 100 && error > -(6144 * kOneQ32) / 100)
            state_.locked = true;
        state_.outputMuted = !state_.locked;
        state_.muteTransitions += !wasMuted && state_.outputMuted;
        state_.unmuteTransitions += wasMuted && !state_.outputMuted;
        if (hard && !hardLatched_) {
            phaseQ32_ = Normalize(static_cast<__int128>(phaseQ32_) + error);
            state_.txCorrectionQ32 =
                Saturate(static_cast<__int128>(state_.txCorrectionQ32) + error);
            state_.phaseRepairApplied = true;
            ++state_.hardResyncRequests;
            ++state_.phaseRepairs;
        }
        hardLatched_ = hard;
        previousFrames_ = in.frames;
        previousTicks_ = in.ticks;
        return true;
    }

  private:
    static constexpr int64_t Normalize(__int128 value) noexcept {
        constexpr int64_t period = int64_t{kTicksPerSecond} * kOneQ32;
        value %= period;
        return static_cast<int64_t>(value < 0 ? value + period : value);
    }
    static constexpr int64_t SignedDifference(__int128 value) noexcept {
        constexpr int64_t period = int64_t{kTicksPerSecond} * kOneQ32;
        const int64_t wrapped = Normalize(value);
        return wrapped > period / 2 ? wrapped - period : wrapped;
    }
    static constexpr int64_t Saturate(__int128 value) noexcept {
        return static_cast<int64_t>(
            std::clamp(value, static_cast<__int128>(std::numeric_limits<int64_t>::min()),
                       static_cast<__int128>(std::numeric_limits<int64_t>::max())));
    }
    uint32_t rate_{0}, offsetFrames_{13}, replayEpoch_{0};
    uint64_t interval_{0}, previousFrames_{0}, lastUpdates_{0};
    int64_t phaseQ32_{0}, previousTicks_{0};
    bool haveReference_{false}, hardLatched_{false};
    State state_{};
};
} // namespace ASFW::Encoding::Motu
