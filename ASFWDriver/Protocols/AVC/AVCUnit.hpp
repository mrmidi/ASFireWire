//
// AVCUnit.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Unit - wraps Discovery::FWUnit with AV/C-specific functionality
// Owns FCPTransport, provides high-level command API, caches probe results
//

#pragma once

#ifdef ASFW_HOST_TEST
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#endif
#include <memory>
#include <vector>
#include "FCPTransport.hpp"
#include "AVCCommands.hpp"
#include "IAVCCommandSubmitter.hpp"
#include "Subunit.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Async/AsyncSubsystem.hpp"
#include "AVCUnitPlugInfoCommand.hpp" // Added include here

namespace ASFW::Protocols::AVC {

//==============================================================================
// Forward Declarations
//==============================================================================
class DescriptorAccessor;

//==============================================================================
// Unit Descriptor Information (Phase 5 Discovery)
//==============================================================================

/// Information extracted from Unit Identifier Descriptor
/// Ref: TA Document 2002013 Section 6.2.1
struct UnitDescriptorInfo {
    // Descriptor sizes from Unit Identifier
    uint8_t generationID{0};
    uint8_t sizeOfListID{0};
    uint8_t sizeOfObjectID{0};
    uint8_t sizeOfEntryPosition{0};

    // Root object lists
    uint16_t numberOfRootObjectLists{0};
    std::vector<uint64_t> rootListIDs;  // Variable-size IDs

    // Traversed root list contents (object IDs in each list)
    struct RootListContents {
        uint64_t listID;
        std::vector<uint64_t> objectIDs;
    };
    std::vector<RootListContents> rootListContents;

    // Support status
    bool descriptorMechanismSupported{false};
};

//==============================================================================
// AV/C Unit
//==============================================================================

class AVCUnit : public std::enable_shared_from_this<AVCUnit>, public IAVCCommandSubmitter {
public:
    AVCUnit(std::shared_ptr<Discovery::FWDevice> device,
            std::shared_ptr<Discovery::FWUnit> unit,
            Async::AsyncSubsystem& asyncSubsystem);

    ~AVCUnit();

    AVCUnit(const AVCUnit&) = delete;
    AVCUnit& operator=(const AVCUnit&) = delete;

    void Initialize(std::function<void(bool success)> completion);

    void ReScan(std::function<void(bool success)> completion);

    void ProbeUnitInfo(std::function<void(bool)> completion);

    virtual void SubmitCommand(const AVCCdb& cdb, AVCCompletion completion);

    void GetPlugInfo(std::function<void(AVCResult, const UnitPlugCounts&)> completion);

    const UnitPlugCounts& GetCachedPlugCounts() const { return plugCounts_; }

    const std::vector<std::shared_ptr<Subunit>>& GetSubunits() const { return subunits_; }

    const UnitDescriptorInfo& GetDescriptorInfo() const { return descriptorInfo_; }

    std::shared_ptr<Discovery::FWUnit> GetFWUnit() const { return unit_.lock(); }

    std::shared_ptr<Discovery::FWDevice> GetDevice() const { return device_.lock(); }

    FCPTransport& GetFCPTransport() { return *fcpTransport_; }
    const FCPTransport& GetFCPTransport() const { return *fcpTransport_; }

    Async::AsyncSubsystem& GetAsyncSubsystem() { return asyncSubsystem_; }

    void OnBusReset(uint32_t newGeneration);

    bool IsInitialized() const { return initialized_; }

    uint64_t GetGUID() const;

    uint32_t GetSpecID() const;

private:
    void ProbeDescriptorMechanism(std::function<void(bool)> completion);

    bool ParseUnitIdentifier(const std::vector<uint8_t>& data);

    void TraverseRootLists(size_t listIndex, std::function<void(bool)> completion);

    void ReadRootObjectList(uint64_t listID,
                           std::function<void(bool success, std::vector<uint64_t> objectIDs)> completion);

    void ProbeSubunits(std::function<void(bool)> completion);

    void ProbePlugs(std::function<void(bool)> completion);

    void ProbeSignalFormat(std::function<void(bool)> completion);

    void StoreSubunitInfo(const AVCSubunitInfoCommand::SubunitInfo& info);

    void ParseSubunitCapabilities(size_t index, std::function<void(bool)> completion);

    std::weak_ptr<Discovery::FWDevice> device_;
    std::weak_ptr<Discovery::FWUnit> unit_;

    Async::AsyncSubsystem& asyncSubsystem_;

    OSSharedPtr<FCPTransport> fcpTransport_;

    std::shared_ptr<DescriptorAccessor> descriptorAccessor_;

    std::vector<std::shared_ptr<Subunit>> subunits_;
    UnitPlugCounts plugCounts_;
    UnitDescriptorInfo descriptorInfo_;

    bool initialized_{false};
};

} // namespace ASFW::Protocols::AVC
