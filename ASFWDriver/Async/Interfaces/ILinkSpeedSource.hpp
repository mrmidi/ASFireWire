#pragma once

#include <optional>

#include "../../Common/FWTypes.hpp"

namespace ASFW::Async {

/**
 * @brief Supplies the empirically observed link speed for a remote node.
 *
 * Self-ID tells us what a node *claims* it can do. It does not tell us what the
 * cable, the repeater chain and the peer's link actually sustain: a node can
 * advertise S400 in Self-ID and acknowledge nothing at that speed (observed on a
 * Midas Venice F24 behind a Thunderbolt bridge — every S400 request returned
 * evt_missing_ack while S200 completed).
 *
 * Transport declares this contract; discovery implements it, so the learned
 * value flows down without transport depending on the discovery layer.
 *
 * Cross-validated with Linux drivers/firewire/core-device.c:615-640, which keeps
 * exactly one speed per device (`fw_device::max_speed`): seeded from the Self-ID
 * value, then probed downwards with test reads until one completes, and
 * thereafter passed to *every* transaction for that device (:557, :958, :977,
 * :1132). The comment at :620-624 names our case directly — "devices with link
 * speed less than PHY speed" and firmwares whose advertised value is wrong.
 */
class ILinkSpeedSource {
  public:
    virtual ~ILinkSpeedSource() = default;

    /**
     * @brief Observed speed ceiling for @p nodeId, or nullopt when unproven.
     *
     * nullopt means "no evidence yet" and must leave the caller's Self-ID value
     * untouched — absence of evidence is not evidence of a slow link. A returned
     * value is a ceiling, not a target: callers take the lower of it and the
     * advertised speed so a stale observation can never raise a node above what
     * the current topology says it supports.
     */
    [[nodiscard]] virtual std::optional<FW::FwSpeed>
    ObservedSpeed(FW::NodeId nodeId) const noexcept = 0;
};

} // namespace ASFW::Async
