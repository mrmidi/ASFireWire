//
// CMPClient.cpp
// ASFWDriver - CMP (Connection Management Procedures)
//
// CMP client implementation for connecting to device's PCR registers.
//

#include "CMPClient.hpp"
#include "../../../Logging/Logging.hpp"
#include <DriverKit/IOLib.h>
#include <os/log.h>

namespace ASFW::CMP {

// ============================================================================
// Constructor / Destructor
// ============================================================================

CMPClient::CMPClient(Async::IFireWireBusOps& busOps)
    : busOps_(busOps)
{
}

CMPClient::~CMPClient() = default;

// ============================================================================
// Configuration
// ============================================================================

void CMPClient::SetDeviceNode(uint8_t nodeId, IRM::Generation generation) {
    deviceNodeId_ = nodeId;
    generation_ = generation;
    
    ASFW_LOG(CMP, "CMPClient: Set device node=%u generation=%u",
             nodeId, generation);
}

// ============================================================================
// Internal Helpers
// ============================================================================

void CMPClient::ReadPCRQuadlet(uint32_t addressLo, PCRReadCallback callback) {
    Async::FWAddress addr{PCRRegisters::kAddressHi, addressLo};
    
    // PCR operations use device's max speed (typically S400)
    // Note: CMP to device PCRs can use full speed (unlike IRM which requires S100)
    FW::FwSpeed speed{2};  // S400
    FW::NodeId node{deviceNodeId_};
    FW::Generation gen{generation_};
    
    ASFW_LOG(CMP, "CMPClient: Reading PCR at 0x%08X (node=%u gen=%u)",
             addressLo, deviceNodeId_, generation_);
    
    busOps_.ReadQuad(gen, node, addr, speed,
        [callback, addressLo](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t hostValue = OSSwapBigToHostInt32(raw);
                
                ASFW_LOG(CMP, "CMPClient: Read PCR 0x%08X = 0x%08X (online=%d p2p=%u ch=%u)",
                         addressLo, hostValue,
                         PCRBits::IsOnline(hostValue),
                         PCRBits::GetP2P(hostValue),
                         PCRBits::GetChannel(hostValue));
                
                callback(true, hostValue);
            } else {
                ASFW_LOG(CMP,
                         "CMPClient: Read PCR 0x%08X failed: status=%{public}s(%u)",
                         addressLo,
                         ASFW::Async::ToString(status),
                         static_cast<unsigned>(status));
                callback(false, 0);
            }
        });
}

void CMPClient::CompareSwapPCR(uint32_t addressLo, uint32_t expected, uint32_t desired,
                                CMPCallback callback) {
    Async::FWAddress addr{PCRRegisters::kAddressHi, addressLo};
    
    FW::FwSpeed speed{2};  // S400
    FW::NodeId node{deviceNodeId_};
    FW::Generation gen{generation_};
    
    // Build CAS operand: [compare_value][swap_value] in big-endian
    std::array<uint8_t, 8> operand;
    uint32_t expectedBE = OSSwapHostToBigInt32(expected);
    uint32_t desiredBE = OSSwapHostToBigInt32(desired);
    std::memcpy(&operand[0], &expectedBE, 4);
    std::memcpy(&operand[4], &desiredBE, 4);
    
    ASFW_LOG(CMP, "CMPClient: Lock PCR 0x%08X: 0x%08X → 0x%08X",
             addressLo, expected, desired);
    
    busOps_.Lock(gen, node, addr, FW::LockOp::kCompareSwap,
        std::span{operand}, 4, speed,
        [callback, expected, desired, addressLo](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t oldValue = OSSwapBigToHostInt32(raw);
                
                bool succeeded = (oldValue == expected);
                if (succeeded) {
                    ASFW_LOG(CMP, "CMPClient: Lock PCR 0x%08X succeeded (0x%08X → 0x%08X)",
                             addressLo, expected, desired);
                    callback(CMPStatus::Success);
                } else {
                    ASFW_LOG(CMP, "CMPClient: Lock PCR 0x%08X contention (expected=0x%08X actual=0x%08X)",
                             addressLo, expected, oldValue);
                    callback(CMPStatus::Failed);
                }
            } else {
                ASFW_LOG(CMP,
                         "CMPClient: Lock PCR 0x%08X failed: status=%{public}s(%u)",
                         addressLo,
                         ASFW::Async::ToString(status),
                         static_cast<unsigned>(status));
                callback(CMPStatus::Failed);
            }
        });
}

// ============================================================================
// oPCR Operations (device→host stream)
// ============================================================================

void CMPClient::ReadOPCR(uint8_t plugNum, PCRReadCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid oPCR plug number %u", plugNum);
        callback(false, 0);
        return;
    }
    
    ReadPCRQuadlet(PCRRegisters::GetOPCRAddress(plugNum), callback);
}

void CMPClient::ConnectOPCR(uint8_t plugNum, CMPCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid oPCR plug number %u", plugNum);
        callback(CMPStatus::Failed);
        return;
    }
    
    ASFW_LOG(CMP, "CMPClient: Connecting oPCR[%u]", plugNum);
    PerformConnect(PCRRegisters::GetOPCRAddress(plugNum), plugNum, std::nullopt, callback);
}

void CMPClient::DisconnectOPCR(uint8_t plugNum, CMPCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid oPCR plug number %u", plugNum);
        callback(CMPStatus::Failed);
        return;
    }
    
    ASFW_LOG(CMP, "CMPClient: Disconnecting oPCR[%u]", plugNum);
    PerformDisconnect(PCRRegisters::GetOPCRAddress(plugNum), plugNum, callback);
}

// ============================================================================
// iPCR Operations (host→device stream)
// ============================================================================

void CMPClient::ReadIPCR(uint8_t plugNum, PCRReadCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid iPCR plug number %u", plugNum);
        callback(false, 0);
        return;
    }
    
    ReadPCRQuadlet(PCRRegisters::GetIPCRAddress(plugNum), callback);
}

void CMPClient::ConnectIPCR(uint8_t plugNum, uint8_t channel, CMPCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid iPCR plug number %u", plugNum);
        callback(CMPStatus::Failed);
        return;
    }
    if (channel > 63) {
        ASFW_LOG(CMP, "CMPClient: Invalid channel %u", channel);
        callback(CMPStatus::Failed);
        return;
    }
    
    ASFW_LOG(CMP, "CMPClient: Connecting iPCR[%u] on channel %u", plugNum, channel);
    PerformConnect(PCRRegisters::GetIPCRAddress(plugNum), plugNum, channel, callback);
}

void CMPClient::DisconnectIPCR(uint8_t plugNum, CMPCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid iPCR plug number %u", plugNum);
        callback(CMPStatus::Failed);
        return;
    }
    
    ASFW_LOG(CMP, "CMPClient: Disconnecting iPCR[%u]", plugNum);
    PerformDisconnect(PCRRegisters::GetIPCRAddress(plugNum), plugNum, callback);
}

// ============================================================================
// Private Implementation
// ============================================================================

void CMPClient::PerformConnect(uint32_t pcrAddress, uint8_t plugNum,
                                std::optional<uint8_t> setChannel, CMPCallback callback) {
    // Step 1: Read current PCR value
    ReadPCRQuadlet(pcrAddress, [this, pcrAddress, plugNum, setChannel, callback](bool success, uint32_t current) {
        if (!success) {
            ASFW_LOG(CMP, "CMPClient: Connect failed - cannot read PCR 0x%08X", pcrAddress);
            callback(CMPStatus::Failed);
            return;
        }
        
        // Step 2: Verify plug is online
        if (!PCRBits::IsOnline(current)) {
            ASFW_LOG(CMP, "CMPClient: Connect failed - plug %u not online (PCR=0x%08X)",
                     plugNum, current);
            callback(CMPStatus::Failed);
            return;
        }
        
        // Step 3: Check p2p count
        uint8_t p2p = PCRBits::GetP2P(current);
        if (p2p >= 3) {
            ASFW_LOG(CMP, "CMPClient: Connect failed - p2p count already max (%u)", p2p);
            callback(CMPStatus::NoResources);
            return;
        }
        
        // Step 4: Compute new value (increment p2p, optionally set channel)
        uint32_t newVal = PCRBits::SetP2P(current, p2p + 1);
        
        if (setChannel.has_value()) {
            // Set channel for iPCR connection
            newVal = (newVal & ~PCRBits::kChannelMask) |
                     (static_cast<uint32_t>(*setChannel) << PCRBits::kChannelShift);
        }
        
        ASFW_LOG(CMP, "CMPClient: Connect PCR 0x%08X: p2p %u→%u (0x%08X → 0x%08X)",
                 pcrAddress, p2p, p2p + 1, current, newVal);
        
        // Step 5: Lock-compare-swap
        CompareSwapPCR(pcrAddress, current, newVal, callback);
    });
}

void CMPClient::PerformDisconnect(uint32_t pcrAddress, uint8_t plugNum, CMPCallback callback) {
    // Step 1: Read current PCR value
    ReadPCRQuadlet(pcrAddress, [this, pcrAddress, plugNum, callback](bool success, uint32_t current) {
        if (!success) {
            ASFW_LOG(CMP, "CMPClient: Disconnect failed - cannot read PCR 0x%08X", pcrAddress);
            callback(CMPStatus::Failed);
            return;
        }
        
        // Step 2: Check p2p count
        uint8_t p2p = PCRBits::GetP2P(current);
        if (p2p == 0) {
            ASFW_LOG(CMP, "CMPClient: Disconnect - p2p already 0, nothing to do");
            callback(CMPStatus::Success);  // Already disconnected
            return;
        }
        
        // Step 3: Compute new value (decrement p2p)
        uint32_t newVal = PCRBits::SetP2P(current, p2p - 1);
        
        ASFW_LOG(CMP, "CMPClient: Disconnect PCR 0x%08X: p2p %u→%u (0x%08X → 0x%08X)",
                 pcrAddress, p2p, p2p - 1, current, newVal);
        
        // Step 4: Lock-compare-swap
        CompareSwapPCR(pcrAddress, current, newVal, callback);
    });
}

} // namespace ASFW::CMP
