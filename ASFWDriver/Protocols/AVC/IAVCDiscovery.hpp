//
//  IAVCDiscovery.hpp
//  ASFWDriver
//
//  Interface for AV/C Discovery
//  Decouples AVCHandler from concrete AVCDiscovery for testing.
//

#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include "../../Discovery/DiscoveryTypes.hpp"

namespace ASFW::Protocols::AVC {

class AVCUnit;
class FCPTransport;
class RemoteAvcSession;

class IAVCDiscovery {
public:
    virtual ~IAVCDiscovery() = default;

    /**
     * @brief Get all AV/C units
     * @return Vector of pointers to AVCUnit instances
     */
    virtual std::vector<AVCUnit*> GetAllAVCUnits() = 0;

    /// Acquire a unit-scoped standards session. The returned owner keeps both
    /// the unit and its per-node FCP transaction domain alive.
    virtual std::shared_ptr<RemoteAvcSession> AcquireRemoteSession(
        Discovery::UnitInstanceId unitId) = 0;

    /**
     * @brief Re-scan all AV/C units
     * Triggers re-initialization for all discovered units.
     */
    virtual void ReScanAllUnits() = 0;

    /// Resolve transient ingress evidence to the strong-ID-owned transport.
    /// The generation and source ID come from the received packet; neither is
    /// used as persistent device identity.
    virtual std::shared_ptr<FCPTransport> AcquireFCPTransportForIngress(
        Discovery::Generation generation, uint16_t sourceID) = 0;
};

} // namespace ASFW::Protocols::AVC
