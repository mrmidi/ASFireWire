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
 *
 * @par Threading contract (load-bearing — do not break)
 * TopologyManager is **confined to the dext's "Default" IODispatchQueue** and is
 * NOT internally synchronized. Every writer and every reader runs on that one
 * serial queue, which is what makes access to @ref latest_ and @ref badIRMFlags_
 * race-free:
 *   - Writers: UpdateFromSelfID / InvalidateForBusReset — driven by the bus-reset
 *     FSM off the OHCI interrupt dispatch source, which is created on the Default
 *     queue (InterruptManager::Initialise <- ctx.workQueue).
 *   - Readers: LatestSnapshot (FireWireBusImpl speed/hop queries; ControllerCore::
 *     LatestTopology used by the user-client handlers and StatusPublisher) and the
 *     bad-IRM flag accessors. The user client's ExternalMethod has no queue of its
 *     own, so it too runs on the Default queue; ROM scanning uses scheduler->Queue()
 *     which is bound to the same Default queue.
 * Because all of the above share one serial queue, LatestSnapshot() may safely
 * return the snapshot by value: the copy never overlaps a concurrent assignment.
 *
 * The side queues that exist (com.asfw.avc.rescan, com.asfw.fcp.timeout,
 * com.asfw.isoch.txverify, and the audio dext) must NOT call into TopologyManager.
 * If a future caller genuinely needs off-Default access (e.g. a dedicated BM/IRM
 * worker queue), the correct fix is to guard latest_/badIRMFlags_ with an IOLock
 * (DriverKit primitive) — not to rely on the by-value copy. Until then, no lock.
 */
class TopologyManager {
  public:
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
