#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "TopologyTypes.hpp"
#include "../Controller/ControllerTypes.hpp"

namespace ASFW::Driver {

class BusResetCoordinator;

class BusManager {
public:
    /**
     * @brief Optional PHY configuration fields to apply immediately before a bus reset.
     */
    struct PhyConfigCommand {
        std::optional<uint8_t> gapCount;
        std::optional<uint8_t> forceRootNodeID;
        std::optional<bool> setContender;
    };

    /**
     * @brief Reason why the bus manager decided to retool gap count.
     *
     * `MismatchForce63` mirrors Apple's early `processSelfIDs()` correction:
     * validated packet-0 gaps disagree, so the conservative corrective target is
     * `gap_count = 63`. The remaining reasons implement the later
     * `finishedBusScan()` stabilization rule.
     */
    enum class GapDecisionReason : uint8_t {
        MismatchForce63 = 0,
        ForcedGap = 1,
        TargetGap = 2,
        ZeroObservedGap = 3,
    };

    /**
     * @brief Typed outcome for gap-count optimization.
     */
    struct GapDecision {
        uint8_t gapCount{0x3F};
        GapDecisionReason reason{GapDecisionReason::MismatchForce63};
    };

    /**
     * @brief Gap-reset state tracked across generations.
     *
     * `lastConfirmedGap` is the most recent stable packet-0 gap observed on an
     * accepted topology. `inFlight` is populated only after the coordinator has
     * successfully dispatched a corrective reset carrying a gap update.
     */
    struct GapState {
        struct InFlightReset {
            uint8_t gapCount{0x3F};
            GapDecisionReason reason{GapDecisionReason::MismatchForce63};
        };

        uint8_t lastConfirmedGap{0x3F};
        std::optional<InFlightReset> inFlight;
    };

    enum class RootPolicy : uint8_t {
        Auto = 0,
        ForceLocal = 1,
        ForceNode = 2,
        Delegate = 3
    };

    struct Config {
        RootPolicy rootPolicy = RootPolicy::Delegate;
        uint8_t forcedRootNodeID = 0xFF;
        bool delegateCycleMaster = true;
        bool enableGapOptimization = false;
        uint8_t forcedGapCount = 0;
        bool forcedGapFlag = false;
    };

    BusManager() = default;
    ~BusManager() = default;

    void SetRootPolicy(RootPolicy policy);
    void SetForcedRootNode(uint8_t nodeID);
    void SetDelegateMode(bool enable);
    void SetGapOptimizationEnabled(bool enable);
    void SetForcedGapCount(uint8_t gapCount);

    const Config& GetConfig() const { return config_; }
    [[nodiscard]] static const char* GapDecisionReasonString(GapDecisionReason reason) noexcept;

    [[nodiscard]] std::optional<PhyConfigCommand> AssignCycleMaster(
        const TopologySnapshot& topology,
        const std::vector<bool>& badIRMFlags);

    /**
     * @brief Decide whether the current validated topology needs a gap retool reset.
     *
     * This implements a two-phase Apple-style policy:
     * - inconsistent packet-0 gaps force an early corrective `gap_count = 63`;
     * - otherwise, stable-bus optimization uses the current target gap together
     *   with the previous programmed gap to avoid unnecessary reset churn.
     */
    [[nodiscard]] std::optional<GapDecision> EvaluateGapPolicy(
        const TopologySnapshot& topology,
        const std::vector<uint32_t>& selfIDs);

    /**
     * @brief Record that a corrective reset carrying `gapCount` was actually dispatched.
     *
     * This is called only after PHY configuration transmission and reset
     * initiation both succeeded.
     */
    void NoteGapResetIssued(uint8_t gapCount, GapDecisionReason reason);

    /**
     * @brief Commit the stable packet-0 gap observed on an accepted topology.
     *
     * Callers must only invoke this when packet-0 gaps are consistent for the
     * accepted generation.
     */
    void NoteStableGapObserved(uint8_t observedGap);

    /**
     * @brief Drop any in-flight corrective target after a dispatch failure.
     */
    void ClearInFlightGapReset();

private:
    Config config_;
    GapState gapState_{};

    static constexpr uint8_t GAP_TABLE[26] = {
        63, 5, 7, 8, 10, 13, 16, 18, 21, 24, 26, 29, 32, 35, 37, 40,
        43, 46, 48, 51, 54, 57, 59, 62, 63, 63
    };

    uint8_t CalculateGapFromHops(uint8_t maxHops) const;
};

} // namespace ASFW::Driver
