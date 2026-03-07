#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "../Controller/ControllerTypes.hpp"
#include "SelfIDCapture.hpp"

namespace ASFW::Driver {

/**
 * @class TopologyManager
 * @brief Builds immutable topology snapshots from validated Self-ID captures.
 *
 * Invalid Self-ID input is treated as a hard topology failure for the current
 * generation. The manager never falls back to a stale snapshot after a reset.
 */
class TopologyManager {
  public:
    enum class TopologyBuildErrorCode : uint8_t {
        InvalidSelfID,
        EmptySequenceSet,
        MissingNodeCoverage,
        NoRootNode,
        TreeValidationFailed,
    };

    struct TopologyBuildError {
        TopologyBuildErrorCode code{TopologyBuildErrorCode::InvalidSelfID};
        std::string detail;
    };

    TopologyManager();

    void Reset();
    /// Discard the current snapshot at bus-reset begin so stale topology is never reused.
    void InvalidateForBusReset();

    /**
     * Build a new immutable topology snapshot from a validated Self-ID capture.
     *
     * Returns a typed error when the capture or resulting tree is not trustworthy.
     * Invalid input never reuses the previous snapshot.
     */
    [[nodiscard]] std::expected<TopologySnapshot, TopologyBuildError>
    UpdateFromSelfID(const SelfIDCapture::Result& result, uint64_t timestamp, uint32_t nodeIDReg);

    [[nodiscard]] std::optional<TopologySnapshot> LatestSnapshot() const;
    [[nodiscard]] std::optional<TopologySnapshot>
    CompareAndSwap(std::optional<TopologySnapshot> previous);

    void MarkNodeAsBadIRM(uint8_t nodeID);

    bool IsNodeBadIRM(uint8_t nodeID) const;

    const std::vector<bool>& GetBadIRMFlags() const { return badIRMFlags_; }

    void ClearBadIRMFlags();

    static std::vector<uint8_t> ExtractGapCounts(const std::vector<uint32_t>& selfIDs);
    [[nodiscard]] static const char* TopologyBuildErrorCodeString(
        TopologyBuildErrorCode code) noexcept;

  private:
    std::optional<TopologySnapshot> latest_;

    /// Per-node bad IRM flags (indexed by node ID, 0-62)
    /// true = node failed IRM verification (read/CAS test)
    std::vector<bool> badIRMFlags_;
};

} // namespace ASFW::Driver
