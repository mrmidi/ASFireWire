#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include "DiscoveryTypes.hpp"

namespace ASFW::Discovery {

// Forward declaration
class FWDevice;

class FWUnit : public std::enable_shared_from_this<FWUnit> {
public:
    enum class State {
        Created,
        Ready,
        Suspended,
        Terminated
    };

    static std::shared_ptr<FWUnit> Create(
        std::shared_ptr<FWDevice> parentDevice,
        uint32_t directoryOffset,
        const std::vector<RomEntry>& entries
    );

    uint32_t GetUnitSpecID() const { return unitSpecId_; }
    uint32_t GetUnitSwVersion() const { return unitSwVersion_; }
    uint32_t GetModelID() const { return modelId_; }
    std::optional<uint32_t> GetLUN() const { return logicalUnitNumber_; }
    uint32_t GetDirectoryOffset() const { return directoryOffset_; }

    std::string_view GetVendorName() const { return vendorName_; }
    std::string_view GetProductName() const { return productName_; }

    std::shared_ptr<FWDevice> GetDevice() const;

    State GetState() const { return state_; }
    bool IsReady() const { return state_ == State::Ready; }
    bool IsSuspended() const { return state_ == State::Suspended; }
    bool IsTerminated() const { return state_ == State::Terminated; }

    bool Matches(uint32_t specId, std::optional<uint32_t> swVersion = {}) const;

    void Publish();
    void Suspend();
    void Resume();
    void Terminate();

private:
    FWUnit(std::shared_ptr<FWDevice> parentDevice, uint32_t directoryOffset);

    void ParseEntries(const std::vector<RomEntry>& entries);

    void ExtractTextLeaves(const std::vector<RomEntry>& entries);

    std::shared_ptr<FWDevice> parentDevice_;

    const uint32_t directoryOffset_;

    uint32_t unitSpecId_{0};
    uint32_t unitSwVersion_{0};
    uint32_t modelId_{0};
    std::optional<uint32_t> logicalUnitNumber_;

    std::string vendorName_;
    std::string productName_;

    State state_{State::Created};
};

} // namespace ASFW::Discovery
