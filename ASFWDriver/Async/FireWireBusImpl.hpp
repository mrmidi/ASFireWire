#pragma once
#include "Interfaces/IAsyncControllerPort.hpp"
#include "Interfaces/IFireWireBus.hpp"

// Forward declare TopologyManager
namespace ASFW::Driver {
class TopologyManager;
}

namespace ASFW::Async {

class ILinkSpeedSource;

/**
 * @brief Concrete implementation of IFireWireBus using an async controller port.
 *
 * Thin adapter that delegates to existing CRTP-based async engine.
 * Cost: One virtual dispatch per operation (negligible vs. actual bus latency).
 *
 * Note: Only implements virtual methods (ReadBlock/WriteBlock/Lock/Cancel/Get*).
 * ReadQuad/WriteQuad are non-virtual helpers in IFireWireBusOps (no override needed).
 */
class FireWireBusImpl final : public IFireWireBus {
  public:
    /**
     * @brief Construct bus facade.
     *
     * @param async Reference to async controller port (must outlive this object)
     * @param topo Reference to topology manager (for speed/hop queries)
     * @param observedSpeeds Optional source of empirically observed link speeds
     *        (must outlive this object). When null, GetSpeed reports the Self-ID
     *        advertised speed unchanged, which is the pre-existing behaviour.
     */
    FireWireBusImpl(IAsyncControllerPort& async, Driver::TopologyManager& topo,
                    const ILinkSpeedSource* observedSpeeds = nullptr);

    // IFireWireBusOps implementation (virtual methods only)
    AsyncHandle ReadBlock(FW::Generation gen, FW::NodeId node, FWAddress addr, uint32_t length,
                          FW::FwSpeed speed, InterfaceCompletionCallback callback) override;
    AsyncHandle WriteBlock(FW::Generation gen, FW::NodeId node, FWAddress addr,
                           std::span<const uint8_t> data, FW::FwSpeed speed,
                           InterfaceCompletionCallback callback) override;
    AsyncHandle Lock(FW::Generation gen, FW::NodeId node, FWAddress addr, FW::LockOp op,
                     std::span<const uint8_t> operand, uint32_t responseLength, FW::FwSpeed speed,
                     InterfaceCompletionCallback callback) override;
    bool Cancel(AsyncHandle handle) override;

    // IFireWireBusInfo implementation
    FW::FwSpeed GetSpeed(FW::NodeId nodeId) const override;
    uint32_t HopCount(FW::NodeId nodeA, FW::NodeId nodeB) const override;
    uint8_t GetGapCount() const override;
    FW::Generation GetGeneration() const override;
    FW::NodeId GetLocalNodeID() const override;

  private:
    // Self-ID advertised speed for a node, ignoring observed evidence.
    [[nodiscard]] FW::FwSpeed AdvertisedSpeed(FW::NodeId nodeId) const;

    IAsyncControllerPort& async_;
    Driver::TopologyManager& topo_;
    const ILinkSpeedSource* observedSpeeds_{nullptr};
};

} // namespace ASFW::Async
