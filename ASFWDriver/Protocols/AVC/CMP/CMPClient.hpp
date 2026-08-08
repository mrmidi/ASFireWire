#pragma once

#include "../../../Async/Interfaces/IFireWireBusInfo.hpp"
#include "../../../Async/Interfaces/IFireWireBusOps.hpp"
#include "../../../Bus/IRM/IRMTypes.hpp"
#include "../../../Discovery/DeviceRegistry.hpp"
#include "PCRCodec.hpp"

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

enum class PCRDirection : uint8_t { kOutput, kInput };

// The identity required for every remote CMP operation. Route validity belongs
// exclusively to DeviceRegistry; callers never synthesize a node/generation.
struct CMPDevice {
    Discovery::DeviceRouteToken route{};

    [[nodiscard]] bool IsValid() const noexcept { return static_cast<bool>(route); }
};

using CMPStatus = IRM::AllocationStatus;
using CMPCallback = std::function<void(CMPStatus status)>;
using PCRReadCallback = std::function<void(bool success, uint32_t value)>;

// Host-side CMP initiator. A lease is keyed by (GUID, PCR direction, plug),
// preventing a disconnect from decrementing another device's p2p count.
class CMPClient {
public:
    CMPClient(Async::IFireWireBusOps& busOps, Async::IFireWireBusInfo& busInfo,
              Discovery::DeviceRegistry& routeRegistry);
    ~CMPClient();

    CMPClient(const CMPClient&) = delete;
    CMPClient& operator=(const CMPClient&) = delete;

    void ReadOMPR(const CMPDevice& device, PCRReadCallback callback);
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

    [[nodiscard]] bool IsRouteCurrent(const Discovery::DeviceRouteToken& route) const noexcept;

    // Bus reset destroys remote PCR state. Drop only local bookkeeping; never
    // issue a BREAK in a new generation for an old connection.
    void InvalidateRoute(const Discovery::DeviceRouteToken& route);
    void InvalidateAllLeasesForBusReset();

private:
    struct LeaseKey {
        Discovery::DeviceRouteToken route;
        PCRDirection direction;
        uint8_t plugNum;
        bool operator==(const LeaseKey&) const = default;
    };
    struct LeaseKeyHash {
        size_t operator()(const LeaseKey& key) const noexcept {
            return std::hash<uint64_t>{}(key.route.guid) ^
                   (std::hash<uint64_t>{}(key.route.deviceIncarnation) << 1U) ^
                   (std::hash<uint64_t>{}(key.route.routeEpoch) << 2U) ^
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
    [[nodiscard]] bool IsCurrent(const CMPDevice& device) const noexcept;

    Async::IFireWireBusOps& busOps_;
    Async::IFireWireBusInfo& busInfo_;
    Discovery::DeviceRegistry& routeRegistry_;
    IOLock* lock_{nullptr};
    std::unordered_map<LeaseKey, Lease, LeaseKeyHash> leases_;
};

} // namespace ASFW::CMP
