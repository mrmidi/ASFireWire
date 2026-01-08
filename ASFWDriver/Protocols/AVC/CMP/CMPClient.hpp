#pragma once

#include "../../../IRM/IRMTypes.hpp"
#include "../../../Async/Interfaces/IFireWireBusOps.hpp"
#include <functional>
#include <cstdint>

namespace ASFW::CMP {

// ============================================================================
// PCR Constants (IEC 61883-1)
// ============================================================================

/// PCR register addresses on device (CSR space)
namespace PCRRegisters {
    constexpr uint16_t kAddressHi = 0xFFFF;  ///< CSR address high word
    
    constexpr uint32_t kOMPR = 0xF0000900;   ///< Output Master Plug Register
    constexpr uint32_t kOPCRBase = 0xF0000904; ///< oPCR[0] base
    constexpr uint32_t kIMPR = 0xF0000980;   ///< Input Master Plug Register
    constexpr uint32_t kIPCRBase = 0xF0000984; ///< iPCR[0] base
    constexpr uint32_t kPCRStride = 4;        ///< 4 bytes per plug
    
    inline uint32_t GetOPCRAddress(uint8_t plug) { return kOPCRBase + plug * kPCRStride; }
    inline uint32_t GetIPCRAddress(uint8_t plug) { return kIPCRBase + plug * kPCRStride; }
}

// ============================================================================
// PCR Bit Fields (IEC 61883-1 §10.7)
// ============================================================================

/// PCR bit masks and shifts
namespace PCRBits {
    // Common to oPCR and iPCR
    constexpr uint32_t kOnlineMask      = 0x80000000;  ///< Bit 31: online
    constexpr uint32_t kBcastMask       = 0x7C000000;  ///< Bits 30-26: broadcast count
    constexpr uint32_t kP2PMask         = 0x03000000;  ///< Bits 25-24: p2p count (2 bits)
    constexpr uint8_t  kP2PShift        = 24;
    constexpr uint32_t kChannelMask     = 0x003F0000;  ///< Bits 21-16: channel
    constexpr uint8_t  kChannelShift    = 16;
    constexpr uint32_t kDataRateMask    = 0x0000C000;  ///< Bits 15-14: data rate
    constexpr uint8_t  kDataRateShift   = 14;
    
    // Extract p2p count from PCR value
    inline uint8_t GetP2P(uint32_t pcr) { return (pcr & kP2PMask) >> kP2PShift; }
    
    // Set p2p count in PCR value
    inline uint32_t SetP2P(uint32_t pcr, uint8_t p2p) {
        return (pcr & ~kP2PMask) | ((static_cast<uint32_t>(p2p) & 0x03) << kP2PShift);
    }
    
    // Extract channel from PCR value
    inline uint8_t GetChannel(uint32_t pcr) { return (pcr & kChannelMask) >> kChannelShift; }
    
    // Check if online
    inline bool IsOnline(uint32_t pcr) { return (pcr & kOnlineMask) != 0; }
}

// ============================================================================
// CMP Status Codes
// ============================================================================

/// CMP operation status (compatible with IRM::AllocationStatus)
using CMPStatus = IRM::AllocationStatus;

/// CMP operation callback
using CMPCallback = std::function<void(CMPStatus status)>;

/// PCR read callback
using PCRReadCallback = std::function<void(bool success, uint32_t value)>;

// ============================================================================
// CMPClient - Connection Management Procedures Client
// ============================================================================

/**
 * CMPClient - Manages CMP connections to remote device's plugs.
 *
 * This is a CMP **client** that connects TO a device's PCR registers.
 * It performs:
 * - Read of oPCR/iPCR registers
 * - Lock-compare-swap to increment/decrement p2p connection count
 *
 * Per IEC 61883-1 §10.8:
 * - CMP ESTABLISH: Increment p2p count (create connection)
 * - CMP BREAK: Decrement p2p count (destroy connection)
 *
 * Usage:
 *   CMPClient cmpClient(busOps);
 *   cmpClient.SetDeviceNode(deviceNodeId, generation);
 *   cmpClient.ConnectOPCR(0, [](CMPStatus status) { ... });
 *
 * Reference: Apple's LockRq to 0xF000.0904 in FireBug logs
 */
class CMPClient {
public:
    /**
     * Construct CMP client with bus operations interface.
     * @param busOps Canonical async bus operations (same as IRMClient)
     */
    explicit CMPClient(Async::IFireWireBusOps& busOps);
    ~CMPClient();
    
    // =========================================================================
    // Configuration
    // =========================================================================
    
    /**
     * Set target device node and generation.
     * Call after topology scan when device node ID is known.
     * @param nodeId Device node ID
     * @param generation Current bus generation
     */
    void SetDeviceNode(uint8_t nodeId, IRM::Generation generation);
    
    /**
     * Get current device node ID.
     * @return Device node ID (0xFF = not set)
     */
    [[nodiscard]] uint8_t GetDeviceNodeID() const { return deviceNodeId_; }
    
    /**
     * Get current generation.
     * @return Bus generation
     */
    [[nodiscard]] IRM::Generation GetGeneration() const { return generation_; }
    
    // =========================================================================
    // oPCR Operations (device→host stream, device transmits)
    // =========================================================================
    
    /**
     * Read oPCR[plugNum] from device.
     * @param plugNum Output plug number (0-30)
     * @param callback Completion with (success, rawValue)
     */
    void ReadOPCR(uint8_t plugNum, PCRReadCallback callback);
    
    /**
     * CMP ESTABLISH on oPCR - connect to device's output plug.
     * Increments p2p connection count via lock-compare-swap.
     * 
     * After success, device should start isochronous transmission.
     * 
     * @param plugNum Output plug number (usually 0)
     * @param callback Completion callback
     */
    void ConnectOPCR(uint8_t plugNum, CMPCallback callback);
    
    /**
     * CMP BREAK on oPCR - disconnect from device's output plug.
     * Decrements p2p connection count via lock-compare-swap.
     * 
     * @param plugNum Output plug number (usually 0)
     * @param callback Completion callback
     */
    void DisconnectOPCR(uint8_t plugNum, CMPCallback callback);
    
    // =========================================================================
    // iPCR Operations (host→device stream, device receives)
    // =========================================================================
    
    /**
     * Read iPCR[plugNum] from device.
     * @param plugNum Input plug number (0-30)
     * @param callback Completion with (success, rawValue)
     */
    void ReadIPCR(uint8_t plugNum, PCRReadCallback callback);
    
    /**
     * CMP ESTABLISH on iPCR - connect to device's input plug.
     * Increments p2p connection count via lock-compare-swap.
     * 
     * After success, device should accept isochronous data we send.
     * 
     * @param plugNum Input plug number (usually 0)
     * @param channel Channel number to set in iPCR
     * @param callback Completion callback
     */
    void ConnectIPCR(uint8_t plugNum, uint8_t channel, CMPCallback callback);
    
    /**
     * CMP BREAK on iPCR - disconnect from device's input plug.
     * Decrements p2p connection count via lock-compare-swap.
     * 
     * @param plugNum Input plug number (usually 0)
     * @param callback Completion callback
     */
    void DisconnectIPCR(uint8_t plugNum, CMPCallback callback);
    
private:
    Async::IFireWireBusOps& busOps_;
    uint8_t deviceNodeId_{0xFF};
    IRM::Generation generation_{0};
    
    // Internal helpers
    void ReadPCRQuadlet(uint32_t addressLo, PCRReadCallback callback);
    void CompareSwapPCR(uint32_t addressLo, uint32_t expected, uint32_t desired, 
                        CMPCallback callback);
    
    // Connect/disconnect implementation (shared logic)
    void PerformConnect(uint32_t pcrAddress, uint8_t plugNum, 
                        std::optional<uint8_t> setChannel, CMPCallback callback);
    void PerformDisconnect(uint32_t pcrAddress, uint8_t plugNum, CMPCallback callback);
};

} // namespace ASFW::CMP
