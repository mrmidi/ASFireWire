#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include "DiscoveryTypes.hpp"
#include "FWUnit.hpp"

namespace ASFW::Discovery {

// Forward declarations
class FWUnit;

class FWDevice : public std::enable_shared_from_this<FWDevice> {
public:
    enum class State {
        Created,
        Ready,
        Quarantined,
        Suspended,
        Terminated
    };

    static std::shared_ptr<FWDevice> Create(
        const DeviceRecord& record,
        const ConfigROM& rom
    );

    DeviceInstanceId GetInstanceId() const { return instanceId_; }
    Guid64 GetObservedGuid() const { return identity_.observedGuid; }
    std::optional<uint32_t> GetRootVendorID() const { return identity_.rootVendorId; }
    std::optional<uint32_t> GetRootModelID() const { return identity_.rootModelId; }
    const DeviceIdentityEvidence& GetIdentityEvidence() const { return identity_; }
    DeviceKind GetKind() const { return kind_; }

    std::string_view GetVendorName() const { return identity_.rootVendorName; }
    std::string_view GetModelName() const { return identity_.rootModelName; }

    Generation GetGeneration() const { return generation_; }
    uint16_t GetNodeID() const { return nodeId_; }
    const LinkPolicy& GetLinkPolicy() const { return linkPolicy_; }

    const std::vector<std::shared_ptr<FWUnit>>& GetUnits() const { return units_; }

    std::vector<std::shared_ptr<FWUnit>> FindUnitsBySpec(
        uint32_t specId,
        std::optional<uint32_t> swVersion = {}
    ) const;

    bool IsAudioCandidate() const { return isAudioCandidate_; }
    bool SupportsAMDTP() const { return supportsAMDTP_; }

    State GetState() const { return state_; }
    bool IsReady() const { return state_ == State::Ready; }
    bool IsQuarantined() const { return state_ == State::Quarantined; }
    QuarantineReason GetQuarantineReason() const { return quarantineReason_; }
    /// Which AV/C command shapes this device may be sent. See
    /// Protocols/AVC/AVCCommandFilter.hpp.
    AvcCommandFilterId GetAvcCommandFilter() const { return avcCommandFilter_; }
    bool IsSuspended() const { return state_ == State::Suspended; }
    bool IsTerminated() const { return state_ == State::Terminated; }

    void Publish();
    void Suspend();
    void Resume(Generation newGen, uint16_t newNodeId, const LinkPolicy& newLink);
    void Terminate();

private:
    FWDevice(const DeviceRecord& record);

    void ParseUnits(const ConfigROM& rom);

    std::vector<RomEntry> ExtractUnitDirectory(
        const ConfigROM& rom,
        uint32_t offsetQuadlets
    ) const;

    const DeviceInstanceId instanceId_;
    DeviceIdentityEvidence identity_;
    const DeviceKind kind_;

    bool isAudioCandidate_{false};
    bool supportsAMDTP_{false};
    QuarantineReason quarantineReason_{QuarantineReason::None};
    AvcCommandFilterId avcCommandFilter_{AvcCommandFilterId::Unrestricted};

    Generation generation_{0};
    uint16_t nodeId_{0xFFFF};
    LinkPolicy linkPolicy_{};
    State state_{State::Created};

    std::vector<std::shared_ptr<FWUnit>> units_;
};

} // namespace ASFW::Discovery
