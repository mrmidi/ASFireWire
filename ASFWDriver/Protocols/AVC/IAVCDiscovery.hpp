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

namespace ASFW::AVC {
class IAvcUnit;
}

namespace ASFW::Protocols::AVC {

class AVCUnit;
class FCPTransport;

class IAVCDiscovery {
public:
    virtual ~IAVCDiscovery() = default;

    /// The unit with this GUID, or null. The caller holds an owning handle: a
    /// unit removed meanwhile stays valid (its transport refuses new frames).
    virtual std::shared_ptr<AVCUnit> Unit(uint64_t guid) = 0;

    /// Every AV/C unit, as owning handles.
    virtual std::vector<std::shared_ptr<AVCUnit>> Units() = 0;

    /// The unit with this GUID if it can carry traffic now: its FireWire unit
    /// is ready and its route is current. Family code receives this handle.
    virtual std::shared_ptr<ASFW::AVC::IAvcUnit> LiveUnit(uint64_t guid) = 0;

    /**
     * @brief Re-scan all AV/C units
     * Triggers re-initialization for all discovered units.
     */
    virtual void ReScanAllUnits() = 0;

    /// Acquire a transport lease for asynchronous response delivery. The caller
    /// keeps the returned owner until it has finished using the transport.
    virtual std::shared_ptr<FCPTransport> AcquireFCPTransportForNodeID(uint16_t nodeID) = 0;
};

} // namespace ASFW::Protocols::AVC
