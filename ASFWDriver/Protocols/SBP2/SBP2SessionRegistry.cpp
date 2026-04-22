#include "SBP2SessionRegistry.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"

#include <cstring>

namespace ASFW::Protocols::SBP2 {

namespace {

class IOLockGuard {
public:
    explicit IOLockGuard(IOLock* lock) : lock_(lock) {
        if (lock_ != nullptr) {
            IOLockLock(lock_);
        }
    }

    ~IOLockGuard() {
        if (lock_ != nullptr) {
            IOLockUnlock(lock_);
        }
    }

    IOLockGuard(const IOLockGuard&) = delete;
    IOLockGuard& operator=(const IOLockGuard&) = delete;

private:
    IOLock* lock_{nullptr};
};

} // namespace

// SCSI INQUIRY CDB (6 bytes)
static void BuildInquiryCDB(uint8_t allocationLength, std::span<uint8_t, 6> cdb) {
    cdb[0] = 0x12;                // OPERATION CODE = INQUIRY
    cdb[1] = 0x00;                // EVPD=0, page code=0
    cdb[2] = 0x00;                // Page code
    cdb[3] = allocationLength;    // Allocation length
    cdb[4] = 0x00;                // Reserved
    cdb[5] = 0x00;                // Control
}

// Build SBP2TargetInfo from FWUnit metadata.
static SBP2TargetInfo BuildTargetInfoFromUnit(const Discovery::FWUnit& unit) {
    SBP2TargetInfo info{};

    info.managementAgentOffset = unit.GetManagementAgentOffset().value_or(0);
    info.lun = static_cast<uint16_t>(unit.GetLUN().value_or(0) & 0xFFFF);

    auto uc = unit.GetUnitCharacteristics();
    if (uc.has_value()) {
        const uint32_t v = *uc;
        const uint8_t orbSizeUnits = static_cast<uint8_t>((v >> 24) & 0xFF);
        const uint8_t timeoutUnits = static_cast<uint8_t>((v >> 16) & 0xFF);
        info.managementTimeoutMs = static_cast<uint32_t>(timeoutUnits) * 500;
        info.maxORBSize = std::max<uint16_t>(static_cast<uint16_t>(orbSizeUnits) * 4, 32);
    }
    info.maxCommandBlockSize = info.maxORBSize > 12
        ? static_cast<uint16_t>(info.maxORBSize - 12) : 0;

    auto device = unit.GetDevice();
    if (device) {
        info.targetNodeId = device->GetNodeID();
    }

    return info;
}

SBP2SessionRegistry::SBP2SessionRegistry(Async::IFireWireBus& bus,
                                         Async::IFireWireBusInfo& busInfo,
                                         AddressSpaceManager& addrSpaceMgr,
                                         Discovery::IDeviceManager& deviceManager,
                                         IODispatchQueue* workQueue)
    : bus_(bus)
    , busInfo_(busInfo)
    , addrSpaceMgr_(addrSpaceMgr)
    , deviceManager_(deviceManager)
    , workQueue_(workQueue) {
    lock_ = IOLockAlloc();
}

SBP2SessionRegistry::~SBP2SessionRegistry() {
    IOLockGuard lock(lock_);
    for (auto& [handle, record] : sessions_) {
        if (record.session) {
            if (record.session->State() == LoginState::LoggedIn) {
                record.session->Logout();
            }
        }
        CleanupInquiryResources(record);
    }
    sessions_.clear();

    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

std::expected<uint64_t, int> SBP2SessionRegistry::CreateSession(void* owner,
                                                                 uint64_t guid,
                                                                 uint32_t romOffset) {
    auto unit = ResolveUnit(guid, romOffset);
    if (!unit) {
        ASFW_LOG(SBP2, "SBP2SessionRegistry: no unit found for guid=0x%016llx romOffset=%u",
                 guid, romOffset);
        return std::unexpected(kIOReturnNotFound);
    }

    if (unit->GetUnitSpecID() != kSBP2UnitSpecId) {
        ASFW_LOG(SBP2, "SBP2SessionRegistry: unit spec 0x%06x is not SBP-2", unit->GetUnitSpecID());
        return std::unexpected(kIOReturnUnsupported);
    }

    auto mgmtOffset = unit->GetManagementAgentOffset();
    if (!mgmtOffset.has_value() || *mgmtOffset == 0) {
        ASFW_LOG(SBP2, "SBP2SessionRegistry: unit has no Management_Agent_Offset");
        return std::unexpected(kIOReturnUnsupported);
    }

    auto targetInfo = BuildTargetInfoFromUnit(*unit);
    if (targetInfo.managementAgentOffset == 0) {
        return std::unexpected(kIOReturnUnsupported);
    }

    auto session = std::make_unique<SBP2LoginSession>(bus_, busInfo_, addrSpaceMgr_);
    session->Configure(targetInfo);
    session->SetWorkQueue(workQueue_);

    IOLockGuard lock(lock_);
    const uint64_t handle = nextHandle_++;

    SBP2SessionRecord record{};
    record.handle = handle;
    record.owner = owner;
    record.guid = guid;
    record.romOffset = romOffset;
    record.session = std::move(session);

    auto [it, inserted] = sessions_.emplace(handle, std::move(record));
    if (!inserted) {
        return std::unexpected(kIOReturnNoMemory);
    }

    ASFW_LOG(SBP2, "SBP2SessionRegistry: created session handle=%llu guid=0x%016llx romOffset=%u",
             handle, guid, romOffset);
    return handle;
}

bool SBP2SessionRegistry::StartLogin(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->session) {
        return false;
    }

    if (record->session->State() != LoginState::Idle) {
        return false;
    }

    record->session->SetLoginCallback([this, handle](const LoginCompleteParams& params) {
        IOLockGuard cbLock(lock_);
        auto* rec = FindByHandle(handle);
        if (!rec) return;

        rec->state.lastError = params.status;
        if (params.status == 0) {
            rec->state.loginID = params.loginResponse.loginID;
            rec->state.loginState = LoginState::LoggedIn;
            rec->state.generation = params.generation;
        } else {
            rec->state.loginState = LoginState::Failed;
        }
    });

    return record->session->Login();
}

std::optional<SBP2SessionState> SBP2SessionRegistry::GetSessionState(uint64_t handle) const {
    IOLockGuard lock(lock_);
    const auto* record = FindByHandle(handle);
    if (!record || !record->session) {
        return std::nullopt;
    }

    SBP2SessionState state{};
    state.loginState = record->session->State();
    state.loginID = record->session->LoginID();
    state.generation = record->session->Generation();
    state.lastError = record->state.lastError;
    state.reconnectPending = (state.loginState == LoginState::Suspended);
    return state;
}

bool SBP2SessionRegistry::SubmitInquiry(uint64_t handle, uint8_t allocationLength) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->session) {
        return false;
    }

    if (record->session->State() != LoginState::LoggedIn) {
        return false;
    }

    if (record->inquiryInFlight) {
        return false;
    }

    // Allocate read buffer
    uint64_t bufHandle{0};
    AddressSpaceManager::AddressRangeMeta bufMeta{};
    const kern_return_t kr = addrSpaceMgr_.AllocateAddressRangeAuto(
        record->owner, 0xFFFF, allocationLength, &bufHandle, &bufMeta);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(SBP2, "SBP2SessionRegistry: failed to allocate inquiry buffer: 0x%08x", kr);
        return false;
    }

    // Build page table
    auto pageTable = std::make_unique<SBP2PageTable>(addrSpaceMgr_, record->owner);
    SBP2PageTable::Segment seg{bufMeta.address, allocationLength};
    if (!pageTable->Build(std::span<const SBP2PageTable::Segment>(&seg, 1),
                          busInfo_.GetLocalNodeID().value)) {
        addrSpaceMgr_.DeallocateAddressRange(record->owner, bufHandle);
        return false;
    }

    // Create ORB
    const uint16_t maxCDB = record->session->TargetInfo().maxCommandBlockSize;
    if (maxCDB < 6) {
        addrSpaceMgr_.DeallocateAddressRange(record->owner, bufHandle);
        return false;
    }

    auto orb = std::make_unique<SBP2CommandORB>(addrSpaceMgr_, record->owner, maxCDB);

    // Set CDB
    std::array<uint8_t, 6> cdb{};
    BuildInquiryCDB(allocationLength, std::span<uint8_t, 6>{cdb});
    orb->SetCommandBlock(std::span<const uint8_t>{cdb.data(), 6});

    // Set flags and page table
    orb->SetFlags(SBP2CommandORB::kNotify | SBP2CommandORB::kDataFromTarget |
                  SBP2CommandORB::kImmediate | SBP2CommandORB::kNormalORB);
    orb->SetMaxPayloadSize(record->session->MaxPayloadSize());
    orb->SetDataDescriptor(pageTable->GetResult());

    const uint64_t captureHandle = handle;
    const uint64_t captureBufHandle = bufHandle;
    const uint8_t captureAllocLen = allocationLength;

    orb->SetCompletionCallback([this, captureHandle, captureBufHandle, captureAllocLen](int status) {
        IOLockGuard cbLock(lock_);
        auto* rec = FindByHandle(captureHandle);
        if (!rec) return;

        rec->inquiryInFlight = false;

        if (status != 0) {
            rec->state.lastError = status;
            return;
        }

        // Read inquiry data from address space buffer
        std::vector<uint8_t> data;
        const kern_return_t kr = addrSpaceMgr_.ReadIncomingData(
            rec->owner, captureBufHandle, 0, captureAllocLen, &data);
        if (kr == kIOReturnSuccess && !data.empty()) {
            rec->inquiryResult = std::move(data);
            rec->inquiryReady = true;
        }
    });

    if (!record->session->SubmitORB(orb.get())) {
        addrSpaceMgr_.DeallocateAddressRange(record->owner, bufHandle);
        return false;
    }

    record->inquiryInFlight = true;
    record->inquiryBufferHandle = bufHandle;
    record->inquiryORB = std::move(orb);
    record->inquiryPageTable = std::move(pageTable);

    return true;
}

std::optional<std::vector<uint8_t>> SBP2SessionRegistry::GetInquiryResult(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto* record = FindByHandle(handle);
    if (!record || !record->inquiryReady) {
        return std::nullopt;
    }

    auto result = std::move(record->inquiryResult);
    record->inquiryResult.clear();
    record->inquiryReady = false;
    CleanupInquiryResources(*record);
    return result;
}

bool SBP2SessionRegistry::ReleaseSession(uint64_t handle) {
    IOLockGuard lock(lock_);
    auto it = sessions_.find(handle);
    if (it == sessions_.end()) {
        return false;
    }

    auto& record = it->second;
    if (record.session) {
        if (record.session->State() == LoginState::LoggedIn) {
            record.session->Logout();
        }
    }

    CleanupInquiryResources(record);
    sessions_.erase(it);
    return true;
}

void SBP2SessionRegistry::ReleaseOwner(void* owner) {
    IOLockGuard lock(lock_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.owner == owner) {
            auto& record = it->second;
            if (record.session && record.session->State() == LoginState::LoggedIn) {
                record.session->Logout();
            }
            CleanupInquiryResources(record);
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void SBP2SessionRegistry::OnBusReset(uint16_t newGeneration) {
    IOLockGuard lock(lock_);
    for (auto& [handle, record] : sessions_) {
        if (record.session) {
            record.session->HandleBusReset(newGeneration);
        }
    }
}

void SBP2SessionRegistry::RefreshTargets(Discovery::Generation gen) {
    IOLockGuard lock(lock_);
    for (auto& [handle, record] : sessions_) {
        if (!record.session) continue;
        if (record.session->State() != LoginState::Suspended) continue;

        // Re-resolve unit to get updated node ID
        auto unit = ResolveUnit(record.guid, record.romOffset);
        if (!unit) {
            ASFW_LOG(SBP2, "SBP2SessionRegistry: RefreshTargets: unit not found for handle=%llu",
                     handle);
            continue;
        }

        // Update target info with fresh node ID
        auto targetInfo = BuildTargetInfoFromUnit(*unit);
        record.session->Configure(targetInfo);

        ASFW_LOG(SBP2, "SBP2SessionRegistry: reconnecting session handle=%llu gen=%u",
                 handle, gen.value);
        record.session->Reconnect();
    }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

SBP2SessionRecord* SBP2SessionRegistry::FindByHandle(uint64_t handle) {
    auto it = sessions_.find(handle);
    return it != sessions_.end() ? &it->second : nullptr;
}

const SBP2SessionRecord* SBP2SessionRegistry::FindByHandle(uint64_t handle) const {
    auto it = sessions_.find(handle);
    return it != sessions_.end() ? &it->second : nullptr;
}

std::shared_ptr<Discovery::FWUnit> SBP2SessionRegistry::ResolveUnit(uint64_t guid,
                                                                      uint32_t romOffset) const {
    auto devices = deviceManager_.GetAllDevices();
    for (const auto& device : devices) {
        if (!device || device->GetGUID() != guid) {
            continue;
        }
        for (const auto& unit : device->GetUnits()) {
            if (unit && unit->GetDirectoryOffset() == romOffset) {
                return unit;
            }
        }
    }
    return nullptr;
}

void SBP2SessionRegistry::CleanupInquiryResources(SBP2SessionRecord& record) {
    if (record.inquiryBufferHandle) {
        addrSpaceMgr_.DeallocateAddressRange(record.owner, record.inquiryBufferHandle);
        record.inquiryBufferHandle = 0;
    }
    record.inquiryORB.reset();
    record.inquiryPageTable.reset();
    record.inquiryInFlight = false;
}

} // namespace ASFW::Protocols::SBP2
