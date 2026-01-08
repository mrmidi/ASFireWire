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
        Suspended,
        Terminated
    };

    static std::shared_ptr<FWDevice> Create(
        const DeviceRecord& record,
        const ConfigROM& rom
    );

    Guid64 GetGUID() const { return guid_; }
    uint32_t GetVendorID() const { return vendorId_; }
    uint32_t GetModelID() const { return modelId_; }
    DeviceKind GetKind() const { return kind_; }

    std::string_view GetVendorName() const { return vendorName_; }
    std::string_view GetModelName() const { return modelName_; }

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

    const Guid64 guid_;
    const uint32_t vendorId_;
    const uint32_t modelId_;
    const DeviceKind kind_;

    std::string vendorName_;
    std::string modelName_;

    bool isAudioCandidate_{false};
    bool supportsAMDTP_{false};

    Generation generation_{0};
    uint16_t nodeId_{0xFFFF};
    LinkPolicy linkPolicy_{};
    State state_{State::Created};

    std::vector<std::shared_ptr<FWUnit>> units_;
};

} // namespace ASFW::Discovery
