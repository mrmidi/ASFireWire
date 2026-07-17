#pragma once

#include "../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../Bus/IRM/IRMTypes.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace ASFW::CMP {

// Canonical remote CMP register layout. Cross-validated with Linux
// sound/firewire/cmp.c:61-70 and Apple IOFireWireAVCUserClient.cpp:766-797.
namespace PCRRegisters {
constexpr uint16_t kAddressHi = 0xFFFF;
constexpr uint32_t kOMPR = 0xF0000900;
constexpr uint32_t kOPCRBase = 0xF0000904;
constexpr uint32_t kIMPR = 0xF0000980;
constexpr uint32_t kIPCRBase = 0xF0000984;
constexpr uint32_t kPCRStride = 4;
inline constexpr uint32_t GetOPCRAddress(uint8_t plug) { return kOPCRBase + plug * kPCRStride; }
inline constexpr uint32_t GetIPCRAddress(uint8_t plug) { return kIPCRBase + plug * kPCRStride; }
} // namespace PCRRegisters

namespace PCRBits {
constexpr uint32_t kOnlineMask = 0x80000000;
constexpr uint32_t kBcastMask = 0x40000000;
constexpr uint32_t kP2PMask = 0x3F000000;
constexpr uint8_t kP2PShift = 24;
constexpr uint32_t kChannelMask = 0x003F0000;
constexpr uint8_t kChannelShift = 16;
constexpr uint32_t kDataRateMask = 0x0000C000; // oPCR only
constexpr uint8_t kDataRateShift = 14;
constexpr uint32_t kOverheadIdMask = 0x00003C00; // oPCR only
constexpr uint8_t kOverheadIdShift = 10;

inline constexpr uint8_t GetP2P(uint32_t pcr) { return (pcr & kP2PMask) >> kP2PShift; }
inline constexpr uint32_t SetP2P(uint32_t pcr, uint8_t p2p) {
    return (pcr & ~kP2PMask) | ((static_cast<uint32_t>(p2p) & 0x3F) << kP2PShift);
}
inline constexpr uint8_t GetChannel(uint32_t pcr) { return (pcr & kChannelMask) >> kChannelShift; }
inline constexpr uint32_t SetChannel(uint32_t pcr, uint8_t channel) {
    return (pcr & ~kChannelMask) | ((static_cast<uint32_t>(channel) & 0x3F) << kChannelShift);
}
inline constexpr uint32_t SetDataRate(uint32_t pcr, FW::FwSpeed speed) {
    return (pcr & ~kDataRateMask) |
           ((static_cast<uint32_t>(speed) & 0x03) << kDataRateShift);
}
inline constexpr uint32_t SetOverheadId(uint32_t pcr, uint8_t overheadId) {
    return (pcr & ~kOverheadIdMask) |
           ((static_cast<uint32_t>(overheadId) & 0x0F) << kOverheadIdShift);
}
inline constexpr bool IsOnline(uint32_t pcr) { return (pcr & kOnlineMask) != 0; }
inline constexpr bool IsInUse(uint32_t pcr) {
    return (pcr & (kBcastMask | kP2PMask)) != 0;
}
} // namespace PCRBits

enum class PCRDirection : uint8_t { kOutput, kInput };

// The identity required for every remote CMP operation. GUID is the lease key;
// node/generation are only a current routing epoch and cannot be global state.
struct CMPDevice {
    uint64_t guid{0};
    FW::NodeId nodeId{0};
    FW::Generation generation{0};

    [[nodiscard]] bool IsValid() const noexcept { return guid != 0 && nodeId.value < 64; }
};

using CMPStatus = IRM::AllocationStatus;
using CMPCallback = std::function<void(CMPStatus status)>;
using PCRReadCallback = std::function<void(bool success, uint32_t value)>;

// Host-side CMP initiator. A lease is keyed by (GUID, PCR direction, plug),
// preventing a disconnect from decrementing another device's p2p count.
class CMPClient {
public:
    CMPClient(Async::IFireWireBusOps& busOps, Async::IFireWireBusInfo& busInfo);
    ~CMPClient();

    CMPClient(const CMPClient&) = delete;
    CMPClient& operator=(const CMPClient&) = delete;

    void ReadOPCR(const CMPDevice& device, uint8_t plugNum, PCRReadCallback callback);
    void ConnectOPCR(const CMPDevice& device, uint8_t plugNum, uint8_t channel, CMPCallback callback);
    void DisconnectOPCR(const CMPDevice& device, uint8_t plugNum, CMPCallback callback);

    void ReadIPCR(const CMPDevice& device, uint8_t plugNum, PCRReadCallback callback);
    void ConnectIPCR(const CMPDevice& device, uint8_t plugNum, uint8_t channel, CMPCallback callback);
    void DisconnectIPCR(const CMPDevice& device, uint8_t plugNum, CMPCallback callback);

    using PCRBoolCallback = std::function<void(bool success, bool used)>;
    void CheckPlugUsed(const CMPDevice& device, PCRDirection dir, uint8_t plugNum,
                       PCRBoolCallback callback);
    void BreakBothConnections(const CMPDevice& device, uint8_t plugNum, CMPCallback callback);

    // Compatibility shims for legacy user-client diagnostics. They deliberately
    // cannot issue bus traffic: an operation without a GUID/node/generation
    // must never select an arbitrary device.
    void ConnectOPCR(uint8_t, uint8_t, CMPCallback callback) { callback(CMPStatus::Failed); }
    void DisconnectOPCR(uint8_t, CMPCallback callback) { callback(CMPStatus::Failed); }
    void ConnectIPCR(uint8_t, uint8_t, CMPCallback callback) { callback(CMPStatus::Failed); }
    void DisconnectIPCR(uint8_t, CMPCallback callback) { callback(CMPStatus::Failed); }

    // Bus reset destroys remote PCR state. Drop only local bookkeeping; never
    // issue a BREAK in a new generation for an old connection.
    void InvalidateDevice(uint64_t guid);

private:
    struct LeaseKey {
        uint64_t guid;
        PCRDirection direction;
        uint8_t plugNum;
        bool operator==(const LeaseKey&) const = default;
    };
    struct LeaseKeyHash {
        size_t operator()(const LeaseKey& key) const noexcept {
            return std::hash<uint64_t>{}(key.guid) ^
                   (static_cast<size_t>(key.direction) << 8U) ^ key.plugNum;
        }
    };
    enum class LeaseState : uint8_t { kConnecting, kConnected, kDisconnecting };
    struct Lease {
        CMPDevice device;
        uint8_t channel;
        LeaseState state;
    };

    using CompareSwapCallback = std::function<void(CMPStatus, uint32_t observed)>;

    static constexpr uint8_t kMaxPlugNumber = 30;
    static constexpr uint8_t kMaxCompareSwapAttempts = 3;

    void ReadQuadlet(const CMPDevice& device, uint32_t address, FW::FwSpeed speed,
                     PCRReadCallback callback);
    void CompareSwap(const CMPDevice& device, uint32_t address, uint32_t expected,
                     uint32_t desired, FW::FwSpeed speed, CompareSwapCallback callback);
    void ReadMPR(const CMPDevice& device, PCRDirection direction, uint8_t plugNum,
                 std::function<void(CMPStatus, FW::FwSpeed)> callback);
    void AttemptConnect(const LeaseKey& key, const CMPDevice& device, uint8_t channel,
                        FW::FwSpeed speed, uint8_t attempt, CMPCallback callback);
    void AttemptDisconnect(const LeaseKey& key, const Lease& lease, uint8_t attempt,
                           CMPCallback callback);

    [[nodiscard]] bool BeginConnect(const LeaseKey& key, const CMPDevice& device, uint8_t channel);
    [[nodiscard]] bool BeginDisconnect(const LeaseKey& key, const CMPDevice& device, Lease& outLease);
    void CompleteConnect(const LeaseKey& key, const CMPDevice& device, uint8_t channel,
                         CMPStatus status, CMPCallback callback);
    void CompleteDisconnect(const LeaseKey& key, CMPStatus status, CMPCallback callback);
    [[nodiscard]] static CMPStatus MapAsyncStatus(Async::AsyncStatus status) noexcept;
    [[nodiscard]] static uint32_t PCRAddress(PCRDirection direction, uint8_t plugNum) noexcept;
    [[nodiscard]] static uint32_t MPRAddress(PCRDirection direction) noexcept;
    [[nodiscard]] static uint8_t OverheadIdForGapCount(uint8_t gapCount) noexcept;

    Async::IFireWireBusOps& busOps_;
    Async::IFireWireBusInfo& busInfo_;
    IOLock* lock_{nullptr};
    std::unordered_map<LeaseKey, Lease, LeaseKeyHash> leases_;
};

} // namespace ASFW::CMP
