#pragma once

// SBP-2 Session Registry — bridges discovery metadata to SBP2LoginSession instances.
// Owns sessions keyed by (guid, romOffset), handles bus-reset suspend/reconnect,
// and provides the INQUIRY command job for the v1 vertical slice.

#include "SBP2LoginSession.hpp"
#include "SBP2CommandORB.hpp"
#include "SBP2PageTable.hpp"
#include "SCSICommandSet.hpp"
#include "AddressSpaceManager.hpp"
#include "../../Discovery/IDeviceManager.hpp"
#include "../../Discovery/DiscoveryTypes.hpp"
#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace ASFW::Protocols::SBP2 {

// SBP-2 Unit_Spec_Id per ANSI INCITS 335-1999
inline constexpr uint32_t kSBP2UnitSpecId = 0x010483;

// Per-session state exposed to UserClient
struct SBP2SessionState {
    LoginState loginState{LoginState::Idle};
    uint16_t loginID{0};
    uint16_t generation{0};
    int32_t lastError{0};
    bool reconnectPending{false};
};

struct SBP2SessionRecord {
    uint64_t handle{0};
    void* owner{nullptr};
    uint64_t guid{0};
    uint32_t romOffset{0};
    std::unique_ptr<SBP2LoginSession> session;
    SBP2SessionState state{};

    std::optional<SCSI::CommandRequest> activeCommandRequest;
    std::optional<SCSI::CommandResult> pendingCommandResult;
    std::optional<uint8_t> activeCommandOpcode;
    std::optional<uint8_t> lastCompletedCommandOpcode;
    bool commandReady{false};
    bool commandInFlight{false};
    std::unique_ptr<SBP2CommandORB> commandORB;
    std::unique_ptr<SBP2PageTable> commandPageTable;
    uint64_t commandBufferHandle{0};
};

class SBP2SessionRegistry {
public:
    SBP2SessionRegistry(Async::IFireWireBus& bus,
                        Async::IFireWireBusInfo& busInfo,
                        AddressSpaceManager& addrSpaceMgr,
                        Discovery::IDeviceManager& deviceManager,
                        IODispatchQueue* workQueue = nullptr);

    ~SBP2SessionRegistry();

    SBP2SessionRegistry(const SBP2SessionRegistry&) = delete;
    SBP2SessionRegistry& operator=(const SBP2SessionRegistry&) = delete;

    // Create a session for (owner, guid, romOffset).
    // Validates the unit is SBP-2 and has Management_Agent_Offset.
    [[nodiscard]] std::expected<uint64_t, int> CreateSession(void* owner,
                                                              uint64_t guid,
                                                              uint32_t romOffset);

    // Start login for a session. Returns false if not in Idle state.
    [[nodiscard]] bool StartLogin(uint64_t handle);

    // Get session state. Returns nullopt if handle not found.
    [[nodiscard]] std::optional<SBP2SessionState> GetSessionState(uint64_t handle) const;

    // Submit SCSI INQUIRY. Returns false if not logged in or inquiry already in-flight.
    [[nodiscard]] bool SubmitInquiry(uint64_t handle, uint8_t allocationLength = 96);

    // Get inquiry result (destructive read). Returns nullopt if not ready.
    [[nodiscard]] std::optional<std::vector<uint8_t>> GetInquiryResult(uint64_t handle);

    // Submit a generic SCSI command. Returns false if not logged in or another command is active.
    [[nodiscard]] bool SubmitCommand(uint64_t handle, const SCSI::CommandRequest& request);

    // Get generic command result (destructive read). Returns nullopt if not ready.
    [[nodiscard]] std::optional<SCSI::CommandResult> GetCommandResult(uint64_t handle);

    // Release a specific session.
    [[nodiscard]] bool ReleaseSession(uint64_t handle);

    // Release all sessions for an owner (best-effort logout + cleanup).
    void ReleaseOwner(void* owner);

    // Bus reset: suspend all active sessions.
    void OnBusReset(uint16_t newGeneration);

    // After discovery completes: refresh target info and reconnect suspended sessions.
    void RefreshTargets(Discovery::Generation gen);

private:
    SBP2SessionRecord* FindByHandle(uint64_t handle);
    const SBP2SessionRecord* FindByHandle(uint64_t handle) const;

    std::shared_ptr<Discovery::FWUnit> ResolveUnit(uint64_t guid, uint32_t romOffset) const;

    void CleanupCommandResources(SBP2SessionRecord& record);

    Async::IFireWireBus& bus_;
    Async::IFireWireBusInfo& busInfo_;
    AddressSpaceManager& addrSpaceMgr_;
    Discovery::IDeviceManager& deviceManager_;
    IODispatchQueue* workQueue_{nullptr};

    IOLock* lock_{nullptr};
    std::map<uint64_t, SBP2SessionRecord> sessions_;
    uint64_t nextHandle_{1};
};

} // namespace ASFW::Protocols::SBP2
