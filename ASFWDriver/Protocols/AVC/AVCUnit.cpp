//
// AVCUnit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Unit implementation
//

#include "AVCUnit.hpp"
#include "../../Logging/Logging.hpp"

using namespace ASFW::Protocols::AVC;

//==============================================================================
// Constructor / Destructor
//==============================================================================

AVCUnit::AVCUnit(std::shared_ptr<Discovery::FWDevice> device,
                 std::shared_ptr<Discovery::FWUnit> unit,
                 Async::AsyncSubsystem& asyncSubsystem)
    : device_(device), unit_(unit), asyncSubsystem_(asyncSubsystem) {

    // Check for custom FCP addresses in Config ROM (optional)
    // For now, use standard addresses
    FCPTransportConfig config;
    config.commandAddress = kFCPCommandAddress;
    config.responseAddress = kFCPResponseAddress;
    config.timeoutMs = kFCPTimeoutInitial;
    config.interimTimeoutMs = kFCPTimeoutAfterInterim;
    config.maxRetries = kFCPMaxRetries;
    config.allowBusResetRetry = false;  // Default: generation-locked

    // Create FCP transport
    fcpTransport_ = std::make_unique<FCPTransport>(asyncSubsystem_,
                                                    *device,
                                                    config);

    os_log_info(log_,
                "AVCUnit: Created for device GUID=%llx, specID=0x%06x",
                GetGUID(), GetSpecID());
}

AVCUnit::~AVCUnit() {
    os_log_info(log_, "AVCUnit: Destroyed (GUID=%llx)", GetGUID());
}

//==============================================================================
// Initialization
//==============================================================================

void AVCUnit::Initialize(std::function<void(bool)> completion) {
    if (initialized_) {
        os_log_info(log_, "AVCUnit: Already initialized");
        completion(true);
        return;
    }

    os_log_info(log_, "AVCUnit: Initializing...");

    // Step 1: Probe subunits
    ProbeSubunits([this, completion](bool success) {
        if (!success) {
            os_log_error(log_, "AVCUnit: Subunit probe failed");
            completion(false);
            return;
        }

        // Step 2: Probe unit-level plugs
        ProbePlugs([this, completion](bool success) {
            initialized_ = success;

            if (success) {
                os_log_info(log_,
                            "AVCUnit: Initialized successfully - "
                            "%zu subunits, %d input plugs, %d output plugs",
                            subunits_.size(),
                            unitPlugInfo_.numDestPlugs,
                            unitPlugInfo_.numSrcPlugs);
            } else {
                os_log_error(log_, "AVCUnit: Plug probe failed");
            }

            completion(success);
        });
    });
}

//==============================================================================
// Subunit Probing
//==============================================================================

void AVCUnit::ProbeSubunits(std::function<void(bool)> completion) {
    // Create SUBUNIT_INFO command (page 0)
    auto cmd = std::make_shared<AVCSubunitInfoCommand>(*fcpTransport_, 0);

    // Submit command
    cmd->Submit([this, completion, cmd](AVCResult result,
                                         const AVCSubunitInfoCommand::SubunitInfo& info) {
        if (!IsSuccess(result)) {
            os_log_error(log_,
                         "AVCUnit: SUBUNIT_INFO failed: result=%d",
                         static_cast<int>(result));
            completion(false);
            return;
        }

        // Store subunit info
        StoreSubunitInfo(info);

        os_log_info(log_,
                    "AVCUnit: Found %zu subunits",
                    subunits_.size());

        completion(true);
    });
}

void AVCUnit::StoreSubunitInfo(const AVCSubunitInfoCommand::SubunitInfo& info) {
    subunits_.clear();

    for (const auto& entry : info.subunits) {
        // For each subunit type reported, enumerate instances
        for (uint8_t id = 0; id <= entry.maxID; id++) {
            AVCSubunit subunit;
            subunit.type = entry.type;
            subunit.id = id;
            // Plug counts will be queried separately if needed
            subunit.numDestPlugs = 0;
            subunit.numSrcPlugs = 0;

            subunits_.push_back(subunit);

            os_log_info(log_,
                        "AVCUnit: Subunit %zu: type=%{public}s (0x%02x), id=%d",
                        subunits_.size() - 1,
                        GetSubunitTypeName(subunit.type),
                        subunit.type,
                        subunit.id);
        }
    }
}

//==============================================================================
// Plug Probing
//==============================================================================

void AVCUnit::ProbePlugs(std::function<void(bool)> completion) {
    // Query unit-level plugs (subunit = 0xFF)
    auto cmd = std::make_shared<AVCPlugInfoCommand>(*fcpTransport_,
                                                     kAVCSubunitUnit);

    cmd->Submit([this, completion, cmd](AVCResult result,
                                         const AVCPlugInfoCommand::PlugInfo& info) {
        if (!IsSuccess(result)) {
            os_log_error(log_,
                         "AVCUnit: PLUG_INFO failed: result=%d",
                         static_cast<int>(result));
            completion(false);
            return;
        }

        // Store plug info
        unitPlugInfo_ = info;

        os_log_info(log_,
                    "AVCUnit: Unit plugs: %d dest (input), %d src (output)",
                    info.numDestPlugs,
                    info.numSrcPlugs);

        completion(true);
    });
}

//==============================================================================
// Command Submission
//==============================================================================

void AVCUnit::SubmitCommand(const AVCCdb& cdb, AVCCompletion completion) {
    auto cmd = std::make_shared<AVCCommand>(*fcpTransport_, cdb);

    // Capture shared_ptr to keep command alive until completion
    cmd->Submit([completion, cmd](AVCResult result, const AVCCdb& response) {
        completion(result, response);
    });
}

void AVCUnit::GetPlugInfo(std::function<void(AVCResult, const PlugInfo&)> completion) {
    if (initialized_) {
        // Return cached result
        completion(AVCResult::kImplementedStable, unitPlugInfo_);
        return;
    }

    // Query device
    auto cmd = std::make_shared<AVCPlugInfoCommand>(*fcpTransport_,
                                                     kAVCSubunitUnit);

    cmd->Submit([completion, cmd](AVCResult result,
                                   const AVCPlugInfoCommand::PlugInfo& info) {
        completion(result, info);
    });
}

//==============================================================================
// Bus Reset Handling
//==============================================================================

void AVCUnit::OnBusReset(uint32_t newGeneration) {
    os_log_info(log_,
                "AVCUnit: Bus reset (generation %u)",
                newGeneration);

    // Forward to FCP transport (will handle pending commands)
    fcpTransport_->OnBusReset(newGeneration);

    // v1: Keep cached state (subunits, plugs rarely change)
    // Caller can re-Initialize() if topology changed

    // v2 improvement: Could invalidate cache on topology change
    // and re-probe automatically
}

//==============================================================================
// Accessors
//==============================================================================

uint64_t AVCUnit::GetGUID() const {
    auto device = device_.lock();
    if (!device) {
        return 0;
    }
    return device->GetGUID();
}

uint32_t AVCUnit::GetSpecID() const {
    auto unit = unit_.lock();
    if (!unit) {
        return 0;
    }
    return unit->GetUnitSpecID();
}
