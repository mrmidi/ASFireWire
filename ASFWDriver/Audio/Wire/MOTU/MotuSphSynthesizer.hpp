// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once
#include "MotuSph.hpp"
#include "MotuBlockLayout.hpp"
#include "../AMDTP/AmdtpRateGeometry.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace ASFW::Encoding::Motu {
// Restore the TX-owned synthesizer from 5a0f1638, with PR #191's raw RX
// clock observation, execution-time seed, absolute-phase seed and limits.
// One owner for accumulator, feedback, lock and repair; no replay fallback.
enum class MotuSphReferenceCause : uint8_t { kNone, kBootstrap, kGenerationChange,
    kAbsolutePhaseSeed, kRxFrameRegression, kRxTickRegression, kNonAdvancingTicks };

class SphSynthesizer final {
public:
    static constexpr int64_t kOneQ32 = int64_t{1} << 32;
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
        uint32_t seedReReferences{0}, observationDrops{0}, dropBridgeNotAdvanced{0};
        MotuSphReferenceCause referenceCause{MotuSphReferenceCause::kNone};
    };
    [[nodiscard]] static constexpr int64_t NominalStepQ32(uint32_t rate) noexcept {
        return rate ? static_cast<int64_t>((uint64_t{kTicksPerSecond} << 32) / rate) : 0;
    }
    [[nodiscard]] static constexpr int64_t FoldAbsolutePhaseErrorTicks(int64_t ticks) noexcept {
        int64_t folded = ticks % kTicksPerCycle;
        if (folded < 0) folded += kTicksPerCycle;
        return folded > kTicksPerCycle / 2 ? folded - kTicksPerCycle : folded;
    }
    [[nodiscard]] static constexpr int64_t FoldAbsolutePhaseErrorTicksAbout(
        int64_t ticks, int64_t center) noexcept {
        return center + FoldAbsolutePhaseErrorTicks(ticks - center);
    }
    [[nodiscard]] bool Configure(uint32_t rate, std::optional<int32_t> setpoint = {}) noexcept {
        rate_ = RateToIndex(rate) >= 0 ? rate : 0;
        setpoint_ = setpoint;
        if (setpoint && (*setpoint > int32_t{kTicksPerSecond / 2} ||
                         *setpoint < -int32_t{kTicksPerSecond / 2})) rate_ = 0;
        const uint64_t packetFrames = rate_ ? AmdtpRateGeometryForSampleRate(rate_)->sytIntervalFrames : 0;
        interval_ = packetFrames ? ((uint64_t{512} * rate_ + 24000 * packetFrames) /
                                  (48000 * packetFrames)) * packetFrames : 0;
        Reset();
        return state_.configured;
    }
    void Reset() noexcept {
        state_ = {};
        state_.configured = rate_ != 0;
        state_.outputMuted = state_.configured;
        state_.muteTransitions = state_.configured ? 1 : 0;
        state_.stepQ32 = state_.nominalStepQ32 = state_.measuredStepQ32 = NominalStepQ32(rate_);
        phaseQ32_ = pendingRepairQ32_ = 0;
        referenceFrames_ = previousFrames_ = lastUpdates_ = 0;
        referenceTicks_ = previousTicks_ = referenceCorrection_ = referenceError_ = 0;
        haveReference_ = haveAbsolute_ = hardLatched_ = false;
    }
    [[nodiscard]] State Current() const noexcept { return state_; }
    [[nodiscard]] uint64_t DecisionIntervalFrames() const noexcept { return interval_; }
    [[nodiscard]] bool SeedOnce(int64_t ticks) noexcept {
        if (!state_.configured || state_.seeded) return false;
        phaseQ32_ = Normalize(static_cast<__int128>(WrapTick(ticks)) * kOneQ32 + pendingRepairQ32_);
        pendingRepairQ32_ = 0;
        state_.seeded = true;
        return true;
    }
    [[nodiscard]] uint32_t NextSph() noexcept {
        if (!state_.seeded) return 0;
        const uint32_t sph = SphFromTick(static_cast<uint32_t>(phaseQ32_ / kOneQ32));
        phaseQ32_ = Normalize(static_cast<__int128>(phaseQ32_) + state_.stepQ32);
        state_.txCorrectionQ32 = Saturate(static_cast<__int128>(state_.txCorrectionQ32) +
                                         state_.stepQ32 - state_.nominalStepQ32);
        return sph;
    }
    // RX ticks are cumulative accepted raw device-SPH differences, divided by
    // RX frames, never by TX frames or replay-rebased timestamps. FFADO uses
    // its receive sync source's ticks/frame: MotuTransmitStreamProcessor.cpp:299.
    [[nodiscard]] bool Observe(uint64_t generation, uint64_t updates, uint64_t frames,
                               int64_t ticks, std::optional<int64_t> phase = {},
                               int64_t phaseCenter = 0) noexcept {
        if (!state_.configured || !generation || !updates || !frames || ticks <= 0) return false;
        if (state_.streamGeneration && generation != state_.streamGeneration) {
            const uint64_t mutes = state_.muteTransitions + !state_.outputMuted;
            const uint64_t unmutes = state_.unmuteTransitions;
            Reset(); // A different start cannot inherit timestamps or a phase reference.
            state_.muteTransitions = mutes;
            state_.unmuteTransitions = unmutes;
        }
        if (haveReference_ && (updates <= lastUpdates_ || frames <= state_.rxFrames || ticks <= state_.rxTicks)) {
            ++state_.observationDrops;
            state_.dropBridgeNotAdvanced += updates == lastUpdates_;
            return false;
        }
        lastUpdates_ = updates;
        state_.streamGeneration = generation;
        state_.rxFrames = frames;
        state_.rxTicks = ticks;
        if (haveReference_ && frames - previousFrames_ < interval_) return false;
        state_.feedbackUpdated = state_.phaseReferenceReset = state_.stepClamped = false;
        state_.hardResyncRequired = state_.phaseRepairApplied = false;
        state_.referenceCause = MotuSphReferenceCause::kNone;
        ++state_.decisions;
        // Keep PR #191's one-time absolute acquisition, including its branch
        // around the conditioned operating point. Later measurements are not
        // silently promoted to a different, continuous control law.
        if (!haveReference_ || (setpoint_ && phase && !haveAbsolute_)) {
            state_.referenceCause = haveReference_ ? MotuSphReferenceCause::kAbsolutePhaseSeed
                                                  : MotuSphReferenceCause::kBootstrap;
            if (setpoint_ && phase) {
                const int64_t center = FoldAbsolutePhaseErrorTicks(phaseCenter - *setpoint_);
                referenceError_ = -FoldAbsolutePhaseErrorTicksAbout(*phase - *setpoint_, center) * kOneQ32;
                haveAbsolute_ = true;
                ++state_.seedReReferences;
            } else referenceError_ = 0;
            referenceFrames_ = previousFrames_ = frames;
            referenceTicks_ = previousTicks_ = ticks;
            referenceCorrection_ = state_.txCorrectionQ32;
            haveReference_ = true;
            state_.phaseReferenceReset = true;
            state_.stepQ32 = state_.measuredStepQ32 = state_.nominalStepQ32;
            state_.phaseErrorQ32 = referenceError_;
            state_.phaseErrorTicks = referenceError_ / kOneQ32;
            return true;
        }
        const uint64_t elapsed = frames - previousFrames_;
        const __int128 measured = (static_cast<__int128>(ticks - previousTicks_) * kOneQ32) / elapsed;
        const __int128 error = static_cast<__int128>(ticks - referenceTicks_) * kOneQ32 -
            static_cast<__int128>(state_.nominalStepQ32) * (frames - referenceFrames_) -
            (static_cast<__int128>(state_.txCorrectionQ32) - referenceCorrection_) + referenceError_;
        const bool hard = error > static_cast<__int128>(4 * kTicksPerCycle) * kOneQ32 ||
                          error < -static_cast<__int128>(4 * kTicksPerCycle) * kOneQ32;
        const __int128 requested = measured + (hard ? 0 : error / (static_cast<__int128>(elapsed) * (state_.locked ? 4 : 1)));
        // 50160 Hz upper tracking rate is captured from the official driver;
        // 41895 Hz lower tracking rate is PR #191 policy, not a measurement.
        const auto limit = [this](uint32_t trackingRate) {
            return static_cast<int64_t>((static_cast<__int128>(kTicksPerSecond) * 48000 * kOneQ32) /
                                        (static_cast<__int128>(rate_) * trackingRate));
        };
        state_.stepQ32 = std::clamp(Saturate(requested), limit(50160), limit(41895));
        state_.stepClamped = state_.stepQ32 != requested;
        state_.measuredStepQ32 = Saturate(measured);
        state_.phaseErrorQ32 = Saturate(error);
        state_.phaseErrorTicks = Saturate(error / kOneQ32);
        state_.feedbackUpdated = true;
        state_.hardResyncRequired = hard;
        const bool wasMuted = state_.outputMuted;
        if (hard) state_.locked = false;
        else if (error < (static_cast<__int128>(6144) * kOneQ32) / 100 &&
                 error > -(static_cast<__int128>(6144) * kOneQ32) / 100) state_.locked = true;
        state_.outputMuted = !state_.locked;
        state_.muteTransitions += !wasMuted && state_.outputMuted;
        state_.unmuteTransitions += wasMuted && !state_.outputMuted;
        if (hard && !hardLatched_) {
            const int64_t repair = state_.phaseErrorQ32;
            state_.txCorrectionQ32 = Saturate(static_cast<__int128>(state_.txCorrectionQ32) + repair);
            if (state_.seeded) phaseQ32_ = Normalize(static_cast<__int128>(phaseQ32_) + repair);
            else pendingRepairQ32_ = Normalize(static_cast<__int128>(pendingRepairQ32_) + repair);
            state_.phaseRepairApplied = true;
            ++state_.hardResyncRequests;
            ++state_.phaseRepairs;
        }
        hardLatched_ = hard;
        previousFrames_ = frames;
        previousTicks_ = ticks;
        return true;
    }
private:
    static constexpr int64_t Normalize(__int128 value) noexcept {
        constexpr int64_t period = int64_t{kTicksPerSecond} * kOneQ32;
        value %= period;
        return static_cast<int64_t>(value < 0 ? value + period : value);
    }
    static constexpr int64_t Saturate(__int128 value) noexcept {
        return static_cast<int64_t>(std::clamp(value,
            static_cast<__int128>(std::numeric_limits<int64_t>::min()),
            static_cast<__int128>(std::numeric_limits<int64_t>::max())));
    }
    uint32_t rate_{0};
    uint64_t interval_{0}, referenceFrames_{0}, previousFrames_{0}, lastUpdates_{0};
    int64_t phaseQ32_{0}, pendingRepairQ32_{0};
    int64_t referenceTicks_{0}, previousTicks_{0}, referenceCorrection_{0}, referenceError_{0};
    std::optional<int32_t> setpoint_{};
    bool haveReference_{false}, haveAbsolute_{false}, hardLatched_{false};
    State state_{};
};
} // namespace ASFW::Encoding::Motu
