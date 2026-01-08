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
    struct PhyConfigCommand {
        std::optional<uint8_t> gapCount;
        std::optional<uint8_t> forceRootNodeID;
        std::optional<bool> setContender;
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
    void SetForcedGapCount(uint8_t gapCount);

    const Config& GetConfig() const { return config_; }

    [[nodiscard]] std::optional<PhyConfigCommand> AssignCycleMaster(
        const TopologySnapshot& topology,
        const std::vector<bool>& badIRMFlags);

    [[nodiscard]] std::optional<PhyConfigCommand> OptimizeGapCount(
        const TopologySnapshot& topology,
        const std::vector<uint32_t>& selfIDs);

private:
    Config config_;
    uint8_t previousGap_ = 0x3F;

    static constexpr uint8_t GAP_TABLE[26] = {
        63, 5, 7, 8, 10, 13, 16, 18, 21, 24, 26, 29, 32, 35, 37, 40,
        43, 46, 48, 51, 54, 57, 59, 62, 63, 63
    };

    std::optional<uint8_t> FindOtherContender(
        const TopologySnapshot& topology,
        uint8_t excludeNodeID
    ) const;

    uint8_t SelectGoodRoot(
        const TopologySnapshot& topology,
        const std::vector<bool>& badIRMFlags,
        uint8_t badIRMNodeID
    ) const;

    [[nodiscard]] PhyConfigCommand BuildPhyConfigCommand(
        std::optional<uint8_t> forceRootNodeID = std::nullopt,
        std::optional<uint8_t> gapCount = std::nullopt) const;

    uint8_t CalculateGapFromHops(uint8_t maxHops) const;
    uint8_t CalculateGapFromPing(uint32_t maxPingNs) const;
    static uint8_t ExtractGapCount(uint32_t selfIDQuad);
    static bool AreGapsConsistent(const std::vector<uint32_t>& selfIDs);
};

} // namespace ASFW::Driver
