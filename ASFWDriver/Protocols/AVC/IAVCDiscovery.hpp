//
//  IAVCDiscovery.hpp
//  ASFWDriver
//
//  Interface for AV/C Discovery
//  Decouples AVCHandler from concrete AVCDiscovery for testing.
//

#pragma once

#include <functional>
#include <vector>
#include <cstdint>

namespace ASFW::Protocols::AVC {

class AVCUnit;
class FCPTransport;

class IAVCDiscovery {
public:
    virtual ~IAVCDiscovery() = default;

    /**
     * @brief Get all AV/C units
     * @return Vector of pointers to AVCUnit instances
     */
    virtual std::vector<AVCUnit*> GetAllAVCUnits() = 0;

    /**
     * @brief Re-scan all AV/C units
     * Triggers re-initialization for all discovered units.
     */
    virtual void ReScanAllUnits() = 0;

    /// Resolve live FCP transport for a node ID.
    virtual FCPTransport* GetFCPTransportForNodeID(uint16_t nodeID) = 0;

    /// Send AV/C INPUT PLUG SIGNAL FORMAT (opcode 0x19) CONTROL command to change sample rate.
    /// @param guid    GUID of the target device.
    /// @param rateHz  Target sample rate in Hz (32000 / 44100 / 48000 / 88200 / 96000 / 176400 / 192000).
    /// @param callback Called on the FCP callback thread: true = device accepted, false = rejected/error/timeout.
    virtual void SendSampleRateCommand(uint64_t guid,
                                       uint32_t rateHz,
                                       std::function<void(bool)> callback) noexcept = 0;
};

} // namespace ASFW::Protocols::AVC
