//
// PCRSpace.cpp
// ASFWDriver - AV/C Protocol Layer
//
// PCR Space implementation
//

#include "PCRSpace.hpp"
#include "../../Common/CallbackUtils.hpp"
#include "../../Logging/Logging.hpp"

#include <array>

using namespace ASFW::Protocols::AVC;

//==============================================================================
// PCR Read
//==============================================================================

void PCRSpace::ReadPCR(PlugType type,
                       uint8_t plugNum,
                       std::function<void(std::optional<PCRValue>)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    if (plugNum > 30) {
        ASFW_LOG_ERROR(Async,
                     "PCRSpace: Invalid plug number %u (max 30)",
                     plugNum);
        Common::InvokeSharedCallback(completionState, std::optional<PCRValue>{});
        return;
    }

    uint64_t pcrAddress = GetPCRAddress(type, plugNum);

    auto device = unit_.GetDevice();
    if (!device) {
        ASFW_LOG_ERROR(Async, "PCRSpace: Device destroyed");
        Common::InvokeSharedCallback(completionState, std::optional<PCRValue>{});
        return;
    }

    const FW::Generation gen = busInfo_.GetGeneration();
    const FW::NodeId node{static_cast<uint8_t>(device->GetNodeID() & 0x3Fu)};
    const Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((pcrAddress >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(pcrAddress & 0xFFFFFFFFU),
    }};

    busOps_.ReadBlock(
        gen,
        node,
        addr,
        4,
        FW::FwSpeed::S100,
        [completionState, pcrAddress](Async::AsyncStatus status, std::span<const uint8_t> response) {
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Async,
                             "PCRSpace: PCR read failed at 0x%llx: status=%d",
                             pcrAddress,
                             static_cast<int>(status));
                Common::InvokeSharedCallback(completionState, std::optional<PCRValue>{});
                return;
            }

            if (response.size() < 4) {
                ASFW_LOG_ERROR(Async,
                             "PCRSpace: PCR read response too short: %zu bytes",
                             response.size());
                Common::InvokeSharedCallback(completionState, std::optional<PCRValue>{});
                return;
            }

            // Decode quadlet (big-endian)
            uint32_t raw = (static_cast<uint32_t>(response[0]) << 24) |
                           (static_cast<uint32_t>(response[1]) << 16) |
                           (static_cast<uint32_t>(response[2]) << 8) |
                           static_cast<uint32_t>(response[3]);

            PCRValue pcr = PCRValue::Decode(raw);

            ASFW_LOG_INFO(Async,
                        "PCRSpace: Read PCR[%llu] = 0x%08x "
                        "(online=%d, channel=%u, p2p=%u)",
                        pcrAddress, raw,
                        pcr.online, pcr.channel, pcr.p2pCount);

            Common::InvokeSharedCallback(completionState, std::optional<PCRValue>{pcr});
        });
}

//==============================================================================
// PCR Update (Atomic Lock)
//==============================================================================

void PCRSpace::UpdatePCR(PlugType type,
                         uint8_t plugNum,
                         const PCRValue& oldValue,
                         const PCRValue& newValue,
                         std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    if (plugNum > 30) {
        ASFW_LOG_ERROR(Async,
                     "PCRSpace: Invalid plug number %u (max 30)",
                     plugNum);
        Common::InvokeSharedCallback(completionState, false);
        return;
    }

    if (!newValue.IsValid()) {
        ASFW_LOG_ERROR(Async,
                     "PCRSpace: Invalid PCR value");
        Common::InvokeSharedCallback(completionState, false);
        return;
    }

    uint64_t pcrAddress = GetPCRAddress(type, plugNum);

    auto device = unit_.GetDevice();
    if (!device) {
        ASFW_LOG_ERROR(Async, "PCRSpace: Device destroyed");
        Common::InvokeSharedCallback(completionState, false);
        return;
    }

    // Encode old and new values (big-endian quadlets)
    uint32_t oldRaw = oldValue.Encode();
    uint32_t newRaw = newValue.Encode();

    std::array<uint8_t, 8> lockData;
    // arg_value (compare): bytes 0-3
    lockData[0] = (oldRaw >> 24) & 0xFF;
    lockData[1] = (oldRaw >> 16) & 0xFF;
    lockData[2] = (oldRaw >> 8) & 0xFF;
    lockData[3] = oldRaw & 0xFF;

    // data_value (swap): bytes 4-7
    lockData[4] = (newRaw >> 24) & 0xFF;
    lockData[5] = (newRaw >> 16) & 0xFF;
    lockData[6] = (newRaw >> 8) & 0xFF;
    lockData[7] = newRaw & 0xFF;

    const FW::Generation gen = busInfo_.GetGeneration();
    const FW::NodeId node{static_cast<uint8_t>(device->GetNodeID() & 0x3Fu)};
    const Async::FWAddress addr{Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((pcrAddress >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(pcrAddress & 0xFFFFFFFFU),
    }};
    const std::span<const uint8_t> operand{lockData.data(), lockData.size()};

    busOps_.Lock(gen,
                 node,
                 addr,
                 FW::LockOp::kCompareSwap,
                 operand,
                 4,
                 FW::FwSpeed::S100,
        [completionState, pcrAddress, oldRaw, newRaw](Async::AsyncStatus status,
                                                std::span<const uint8_t> response) {
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Async,
                             "PCRSpace: PCR lock failed at 0x%llx: status=%d",
                             pcrAddress,
                             static_cast<int>(status));
                Common::InvokeSharedCallback(completionState, false);
                return;
            }

            if (response.size() < 4) {
                ASFW_LOG_ERROR(Async,
                             "PCRSpace: PCR lock response too short: %zu bytes",
                             response.size());
                Common::InvokeSharedCallback(completionState, false);
                return;
            }

            // Response contains old value (before swap)
            uint32_t actualOld = (static_cast<uint32_t>(response[0]) << 24) |
                                 (static_cast<uint32_t>(response[1]) << 16) |
                                 (static_cast<uint32_t>(response[2]) << 8) |
                                 static_cast<uint32_t>(response[3]);

            if (actualOld != oldRaw) {
                ASFW_LOG_ERROR(Async,
                             "PCRSpace: PCR lock compare failed: "
                             "expected 0x%08x, got 0x%08x",
                             oldRaw, actualOld);
                Common::InvokeSharedCallback(completionState, false);
                return;
            }

            ASFW_LOG_INFO(Async,
                        "PCRSpace: Updated PCR[%llu]: 0x%08x → 0x%08x",
                        pcrAddress, oldRaw, newRaw);

            Common::InvokeSharedCallback(completionState, true);
        });
}

//==============================================================================
// Connection Management
//==============================================================================

void PCRSpace::CreateConnection(uint8_t plugNum,
                                 PlugType plugType,
                                 std::function<void(std::optional<uint8_t>)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    // Step 1: Read current PCR value
    ReadPCR(plugType, plugNum, [this, plugNum, plugType, completionState](std::optional<PCRValue> currentPCR) {
        if (!currentPCR) {
            ASFW_LOG_ERROR(Async,
                         "PCRSpace: Failed to read PCR for connection");
            Common::InvokeSharedCallback(completionState, std::optional<uint8_t>{});
            return;
        }

        // TODO: Implement IRM channel allocation
        // Current IRMAllocationManager API requires a specific channel number,
        // but PCRSpace needs auto-allocation (any available channel).
        // Need to either:
        // 1. Add auto-allocation support to IRM layer, or
        // 2. Implement channel scanning logic here
        //
        // For now, stub out with a placeholder channel
        ASFW_LOG_ERROR(Async,
                     "PCRSpace: IRM allocation not yet implemented");
        Common::InvokeSharedCallback(completionState, std::optional<uint8_t>{});
    });
}

void PCRSpace::DestroyConnection(uint8_t plugNum,
                                  PlugType plugType,
                                  uint8_t channel,
                                  std::function<void(bool)> completion) {
    auto completionState = Common::ShareCallback(std::move(completion));
    // Step 1: Read current PCR value
    ReadPCR(plugType, plugNum, [this, plugNum, plugType, channel, completionState](std::optional<PCRValue> currentPCR) {
        if (!currentPCR) {
            ASFW_LOG_ERROR(Async,
                         "PCRSpace: Failed to read PCR for disconnection");
            Common::InvokeSharedCallback(completionState, false);
            return;
        }

        // Step 2: Update PCR (decrement p2pCount, clear channel if count==0)
        PCRValue newPCR = *currentPCR;

        if (newPCR.p2pCount > 0) {
            newPCR.p2pCount--;
        }

        if (newPCR.p2pCount == 0) {
            newPCR.online = false;
            newPCR.channel = 63;  // No channel
        }

        uint32_t bandwidth = CalculateBandwidth();

        UpdatePCR(plugType, plugNum, *currentPCR, newPCR,
            [this, channel, bandwidth, completionState](bool success) {
                if (!success) {
                    ASFW_LOG_ERROR(Async,
                                 "PCRSpace: Failed to update PCR for disconnection");
                    Common::InvokeSharedCallback(completionState, false);
                    return;
                }

                // TODO: Implement IRM resource release
                // See CreateConnection TODO - IRM layer needs to be integrated
                (void)channel;
                (void)bandwidth;

                ASFW_LOG_INFO(Async,
                            "PCRSpace: Connection destroyed (channel %u - IRM release not implemented)",
                            channel);

                Common::InvokeSharedCallback(completionState, true);
            });
    });
}
