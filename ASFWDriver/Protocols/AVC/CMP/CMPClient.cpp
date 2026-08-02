//
// CMPClient.cpp
// ASFWDriver - CMP (Connection Management Procedures)
//
// CMP client implementation for connecting to device's PCR registers.
//

#include "CMPClient.hpp"
#include "../../../Common/CallbackUtils.hpp"
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
    packedTarget_.store(PackTarget(Target{}), std::memory_order_relaxed);
}

CMPClient::~CMPClient() = default;

// ============================================================================
// Configuration
// ============================================================================

uint64_t CMPClient::PackTarget(Target target) noexcept {
    return static_cast<uint64_t>(target.nodeId) |
           (static_cast<uint64_t>(target.generation.value) << 8) |
           (static_cast<uint64_t>(static_cast<uint8_t>(target.speed)) << 40);
}

CMPClient::Target CMPClient::LoadTarget() const noexcept {
    const uint64_t packed = packedTarget_.load(std::memory_order_acquire);
    return Target{
        .nodeId = static_cast<uint8_t>(packed & 0xFFu),
        .generation = IRM::Generation{
            static_cast<uint32_t>((packed >> 8) & 0xFFFFFFFFu)},
        .speed = static_cast<FW::FwSpeed>((packed >> 40) & 0xFFu),
    };
}

void CMPClient::SetDeviceNode(uint8_t nodeId,
                              IRM::Generation generation,
                              FW::FwSpeed speed) {
    packedTarget_.store(PackTarget(Target{nodeId, generation, speed}),
                        std::memory_order_release);
    
    ASFW_LOG(CMP, "CMPClient: Set device node=%u generation=%u speed=%u",
             nodeId, generation.value, static_cast<unsigned>(speed));
}

uint8_t CMPClient::GetDeviceNodeID() const {
    return LoadTarget().nodeId;
}

IRM::Generation CMPClient::GetGeneration() const {
    return LoadTarget().generation;
}

// ============================================================================
// Internal Helpers
// ============================================================================

void CMPClient::ReadPCRQuadlet(Target target,
                               uint32_t addressLo,
                               PCRReadCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = PCRRegisters::kAddressHi,
        .addressLo = addressLo,
    }};
    
    // PCR operations use device's max speed (typically S400)
    // Note: CMP to device PCRs can use full speed (unlike IRM which requires S100)
    FW::NodeId node{target.nodeId};
    FW::Generation gen{target.generation};
    
    ASFW_LOG(CMP, "CMPClient: Reading PCR at 0x%08X (node=%u gen=%u)",
             addressLo, target.nodeId, target.generation.value);
    
    busOps_.ReadQuad(gen, node, addr, target.speed,
        [callbackState, addressLo](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t hostValue = OSSwapBigToHostInt32(raw);
                
                ASFW_LOG(CMP, "CMPClient: Read PCR 0x%08X = 0x%08X (online=%d p2p=%u ch=%u)",
                         addressLo, hostValue,
                         PCRBits::IsOnline(hostValue),
                         PCRBits::GetP2P(hostValue),
                         PCRBits::GetChannel(hostValue));
                
                Common::InvokeSharedCallback(callbackState, true, hostValue);
            } else {
                ASFW_LOG(CMP,
                         "CMPClient: Read PCR 0x%08X failed: status=%{public}s(%u)",
                         addressLo,
                         ASFW::Async::ToString(status),
                         static_cast<unsigned>(status));
                Common::InvokeSharedCallback(callbackState, false, 0u);
            }
        });
}

void CMPClient::CompareSwapPCR(Target target,
                               uint32_t addressLo,
                               uint32_t expected,
                               uint32_t desired,
                               CMPCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = PCRRegisters::kAddressHi,
        .addressLo = addressLo,
    }};
    
    FW::NodeId node{target.nodeId};
    FW::Generation gen{target.generation};
    
    // Build CAS operand: [compare_value][swap_value] in big-endian
    std::array<uint8_t, 8> operand;
    uint32_t expectedBE = OSSwapHostToBigInt32(expected);
    uint32_t desiredBE = OSSwapHostToBigInt32(desired);
    std::memcpy(&operand[0], &expectedBE, 4);
    std::memcpy(&operand[4], &desiredBE, 4);
    
    ASFW_LOG(CMP, "CMPClient: Lock PCR 0x%08X: 0x%08X → 0x%08X",
             addressLo, expected, desired);
    
    busOps_.Lock(gen, node, addr, FW::LockOp::kCompareSwap,
        std::span{operand}, 4, target.speed,
        [callbackState, expected, desired, addressLo](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            if (status == Async::AsyncStatus::kSuccess && payload.size() == 4) {
                uint32_t raw = 0;
                std::memcpy(&raw, payload.data(), sizeof(raw));
                uint32_t oldValue = OSSwapBigToHostInt32(raw);
                
                bool succeeded = (oldValue == expected);
                if (succeeded) {
                    ASFW_LOG(CMP, "CMPClient: Lock PCR 0x%08X succeeded (0x%08X → 0x%08X)",
                             addressLo, expected, desired);
                    Common::InvokeSharedCallback(callbackState, CMPStatus::Success);
                } else {
                    ASFW_LOG(CMP, "CMPClient: Lock PCR 0x%08X contention (expected=0x%08X actual=0x%08X)",
                             addressLo, expected, oldValue);
                    Common::InvokeSharedCallback(callbackState, CMPStatus::Failed);
                }
            } else {
                ASFW_LOG(CMP,
                         "CMPClient: Lock PCR 0x%08X failed: status=%{public}s(%u)",
                         addressLo,
                         ASFW::Async::ToString(status),
                         static_cast<unsigned>(status));
                Common::InvokeSharedCallback(callbackState, CMPStatus::Failed);
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
    
    ReadPCRQuadlet(LoadTarget(), PCRRegisters::GetOPCRAddress(plugNum),
                   std::move(callback));
}

void CMPClient::ReadOMPR(PCRReadCallback callback) {
    ReadPCRQuadlet(LoadTarget(), PCRRegisters::kOMPR, std::move(callback));
}

void CMPClient::ConnectOPCR(uint8_t plugNum, CMPCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid oPCR plug number %u", plugNum);
        callback(CMPStatus::Failed);
        return;
    }
    
    ASFW_LOG(CMP, "CMPClient: Connecting oPCR[%u]", plugNum);
    PerformConnect(LoadTarget(), PCRRegisters::GetOPCRAddress(plugNum),
                   plugNum, std::nullopt, std::move(callback));
}

void CMPClient::ConnectOPCR(uint8_t plugNum,
                            uint8_t channel,
                            CMPCallback callback) {
    if (plugNum > 30 || channel > 63) {
        callback(CMPStatus::Failed);
        return;
    }
    PerformConnect(LoadTarget(), PCRRegisters::GetOPCRAddress(plugNum), plugNum,
                   channel, std::move(callback));
}

void CMPClient::DisconnectOPCR(uint8_t plugNum, CMPCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid oPCR plug number %u", plugNum);
        callback(CMPStatus::Failed);
        return;
    }
    
    ASFW_LOG(CMP, "CMPClient: Disconnecting oPCR[%u]", plugNum);
    PerformDisconnect(LoadTarget(), PCRRegisters::GetOPCRAddress(plugNum),
                      plugNum, std::move(callback));
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
    
    ReadPCRQuadlet(LoadTarget(), PCRRegisters::GetIPCRAddress(plugNum),
                   std::move(callback));
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
    PerformConnect(LoadTarget(), PCRRegisters::GetIPCRAddress(plugNum),
                   plugNum, channel, std::move(callback));
}

void CMPClient::DisconnectIPCR(uint8_t plugNum, CMPCallback callback) {
    if (plugNum > 30) {
        ASFW_LOG(CMP, "CMPClient: Invalid iPCR plug number %u", plugNum);
        callback(CMPStatus::Failed);
        return;
    }
    
    ASFW_LOG(CMP, "CMPClient: Disconnecting iPCR[%u]", plugNum);
    PerformDisconnect(LoadTarget(), PCRRegisters::GetIPCRAddress(plugNum),
                      plugNum, std::move(callback));
}

// ============================================================================
// Private Implementation
// ============================================================================

void CMPClient::PerformConnect(Target target,
                               uint32_t pcrAddress,
                               uint8_t plugNum,
                                std::optional<uint8_t> setChannel, CMPCallback callback) {
    // Step 1: Read current PCR value
    ReadPCRQuadlet(target, pcrAddress,
        [this, target, pcrAddress, plugNum, setChannel, callback](bool success, uint32_t current) {
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
        if (p2p >= 63) {
            ASFW_LOG(CMP, "CMPClient: Connect failed - p2p count already max (%u)", p2p);
            callback(CMPStatus::NoResources);
            return;
        }
        
        // Step 4: Compute new value (increment p2p, optionally set channel)
        uint32_t newVal = PCRBits::SetP2P(current, p2p + 1);
        
        if (setChannel.has_value()) {
            // Set channel for iPCR connection
            newVal = PCRBits::SetChannel(newVal, *setChannel);
        }
        
        ASFW_LOG(CMP, "CMPClient: Connect PCR 0x%08X: p2p %u→%u (0x%08X → 0x%08X)",
                 pcrAddress, p2p, p2p + 1, current, newVal);
        
        // Step 5: Lock-compare-swap
        CompareSwapPCR(target, pcrAddress, current, newVal, callback);
    });
}

void CMPClient::PerformDisconnect(Target target,
                                  uint32_t pcrAddress,
                                  uint8_t plugNum,
                                  CMPCallback callback) {
    // Step 1: Read current PCR value
    ReadPCRQuadlet(target, pcrAddress,
        [this, target, pcrAddress, plugNum, callback](bool success, uint32_t current) {
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
        CompareSwapPCR(target, pcrAddress, current, newVal, callback);
    });
}

} // namespace ASFW::CMP
