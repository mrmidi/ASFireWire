//
//  IsochHandler.cpp
//  ASFWDriver
//
//  Handler for Isochronous Operations
//

#include "IsochHandler.hpp"
#include <DriverKit/IOUserClient.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include "ASFWDriver.h" // Generated header from .iig
#include "../../Controller/ControllerCore.hpp"
#include "../../IRM/IRMClient.hpp"
#include "../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Protocols/AVC/AVCDiscovery.hpp"
#include "../../Protocols/AVC/StreamFormats/AVCSignalFormatCommand.hpp"
#include "../../Shared/SharedDataModels.hpp"
#include "../../Isoch/IsochReceiveContext.hpp"

namespace ASFW::UserClient {

IsochHandler::IsochHandler(::ASFWDriver* driver)
    : driver_(driver)
{
}

// ============================================================================
// IRM Test Methods
// ============================================================================

// ============================================================================
// IRM Test Methods
// ============================================================================

    // FindMusicSubunit removed - using Unit Plug commands directly.

kern_return_t IsochHandler::TestIRMAllocation(IOUserClientMethodArguments* args) {
    ASFW_LOG(UserClient, "TestIRMAllocation: Starting Configuration & Allocation Sequence");

    auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(driver_->GetControllerCore());
    if (!controllerCore) return kIOReturnNotReady;

    auto* irmClient = controllerCore->GetIRMClient();
    if (!irmClient) return kIOReturnNotReady;
    
    // 1. Get AVC Unit to set Sample Rate
    // Note: We scan for the first available AVC unit for this test
    auto* avcDiscovery = controllerCore->GetAVCDiscovery();
    auto units = avcDiscovery->GetAllAVCUnits();
    if (units.empty()) {
         ASFW_LOG(UserClient, "❌ No AVC Unit found for sample rate configuration.");
         return kIOReturnNotFound;
    }
    auto* avcUnit = units[0]; // Assume first unit is target
    

    // 2. Set Sample Rate to 48kHz using Unit Plug Signal Format (Oxford/Linux style)
    // The Linux driver sets format on Unit Plug 0 (Input and Output).
    // Opcode 0x19 (Input Endpoint) / 0x18 (Output Endpoint)
    // Subunit: 0xFF (Unit)
    
    // We will try setting Input Plug 0 to 48kHz.
    
    ASFW_LOG(UserClient, "Step 1: Setting Unit Plug 0 to 48kHz (Oxford style)...");

    ASFW::Protocols::AVC::AVCCdb cdb;
    cdb.ctype = static_cast<uint8_t>(ASFW::Protocols::AVC::AVCCommandType::kControl);
    cdb.subunit = 0xFF; // Unit
    cdb.opcode = 0x19;  // INPUT PLUG SIGNAL FORMAT
    
    cdb.operands[0] = 0x00; // Plug 0
    cdb.operands[1] = 0x90; // AM824
    cdb.operands[2] = 0x02; // 48kHz (Standard FDF/SFC code) - Confirmed by Golden Log
    cdb.operands[3] = 0xFF; // Padding/Sync
    cdb.operands[4] = 0xFF; // Padding/Sync
    cdb.operandLength = 5;
    
    // Use shared_ptr to ensure valid shared_from_this() logic
    auto cmd = std::make_shared<ASFW::Protocols::AVC::AVCCommand>(
        avcUnit->GetFCPTransport(),
        cdb
    );
    
    cmd->Submit([irmClient, driver=driver_, cmd](ASFW::Protocols::AVC::AVCResult result, const ASFW::Protocols::AVC::AVCCdb& response) {
        if (!ASFW::Protocols::AVC::IsSuccess(result)) {
             ASFW_LOG(UserClient, "❌ Failed to set 48kHz on Unit Plug 0: %d", static_cast<int>(result));
             // Fallback or abort? Let's try Output Plug if Input failed, or just abort.
             return;
        }
        
        ASFW_LOG(UserClient, "✅ Set 48kHz on Unit Plug 0 Success. Proceeding to IRM Allocation.");
        
        // 3. Allocate Resources (Bandwidth for 48kHz)
        constexpr uint8_t kTestChannel = 0;
        constexpr uint32_t kAllocationUnits = 100; 

        ASFW_LOG(UserClient, "Step 2: Allocating Channel %u + %u BW units", kTestChannel, kAllocationUnits);

        irmClient->AllocateResources(kTestChannel, kAllocationUnits,
            [](ASFW::IRM::AllocationStatus status) {
                if (status == ASFW::IRM::AllocationStatus::Success) {
                    ASFW_LOG(UserClient, "✅ IRM allocation succeeded!");
                } else {
                    ASFW_LOG(UserClient, "❌ IRM allocation failed: %d", static_cast<int>(status));
                }
            });
    });

    return kIOReturnSuccess;
}

kern_return_t IsochHandler::TestIRMRelease(IOUserClientMethodArguments* args) {
    ASFW_LOG(UserClient, "TestIRMRelease called");

    auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(driver_->GetControllerCore());
    if (!controllerCore) return kIOReturnNotReady;

    auto* irmClient = controllerCore->GetIRMClient();
    if (!irmClient) return kIOReturnNotReady;

    constexpr uint8_t kTestChannel = 0;
    constexpr uint32_t kTestBandwidth = 84;

    ASFW_LOG(UserClient, "TestIRMRelease: Releasing channel=%u, bandwidth=%u", kTestChannel, kTestBandwidth);

    irmClient->ReleaseResources(kTestChannel, kTestBandwidth,
        [](ASFW::IRM::AllocationStatus status) {
            if (status == ASFW::IRM::AllocationStatus::Success) {
                ASFW_LOG(UserClient, "✅ IRM release succeeded!");
            } else {
                ASFW_LOG(UserClient, "❌ IRM release failed: %d", static_cast<int>(status));
            }
        });

    return kIOReturnSuccess;
}

// ============================================================================
// CMP Test Methods (with Auto-Start)
// ============================================================================

kern_return_t IsochHandler::TestCMPConnectOPCR(IOUserClientMethodArguments* args) {
    ASFW_LOG(UserClient, "TestCMPConnectOPCR called");

    auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(driver_->GetControllerCore());
    if (!controllerCore) return kIOReturnNotReady;

    auto* cmpClient = controllerCore->GetCMPClient();
    if (!cmpClient) return kIOReturnNotReady;

    constexpr uint8_t kTestPlug = 0;
    ASFW_LOG(UserClient, "TestCMPConnectOPCR: Connecting oPCR[%u]", kTestPlug);

    // Use weak ptr or capture 'this' carefully? 'this' is IsochHandler, owned by UserClient.
    // UserClient keeps driver alive? No, UserClient holds OSSharedPtr<ASFWDriver> typically.
    // The callback might outlive the UserClient request if async?
    // CMPClient callbacks are generally executed on WorkLoop.
    // We capture driver pointer.

    auto* driver = driver_; 

    cmpClient->ConnectOPCR(kTestPlug,
        [driver](ASFW::CMP::CMPStatus status) {
            if (status == ASFW::CMP::CMPStatus::Success) {
                ASFW_LOG(UserClient, "✅ CMP oPCR connect succeeded!");
                
                // AUTO-START ISOCH RECEIVE
                // Hardcode Channel 0 for now as per test requirement
                ASFW_LOG(UserClient, "[Auto-Start] Triggering Isoch Receive on Channel 0...");
                driver->StartIsochReceive(0);
                
            } else {
                ASFW_LOG(UserClient, "❌ CMP oPCR connect failed: %d", static_cast<int>(status));
            }
        });

    return kIOReturnSuccess;
}

kern_return_t IsochHandler::TestCMPDisconnectOPCR(IOUserClientMethodArguments* args) {
    ASFW_LOG(UserClient, "TestCMPDisconnectOPCR called");

    auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(driver_->GetControllerCore());
    if (!controllerCore) return kIOReturnNotReady;

    auto* cmpClient = controllerCore->GetCMPClient();
    if (!cmpClient) return kIOReturnNotReady;

    constexpr uint8_t kTestPlug = 0;
    ASFW_LOG(UserClient, "TestCMPDisconnectOPCR: Disconnecting oPCR[%u]", kTestPlug);

    auto* driver = driver_;

    cmpClient->DisconnectOPCR(kTestPlug,
        [driver](ASFW::CMP::CMPStatus status) {
             if (status == ASFW::CMP::CMPStatus::Success) {
                ASFW_LOG(UserClient, "✅ CMP oPCR disconnect succeeded!");
                
                // AUTO-STOP ISOCH RECEIVE
                ASFW_LOG(UserClient, "[Auto-Stop] Stopping Isoch Receive...");
                driver->StopIsochReceive();
                
            } else {
                ASFW_LOG(UserClient, "❌ CMP oPCR disconnect failed: %d", static_cast<int>(status));
            }
        });

    return kIOReturnSuccess;
}

kern_return_t IsochHandler::TestCMPConnectIPCR(IOUserClientMethodArguments* args) {
    ASFW_LOG(UserClient, "TestCMPConnectIPCR called");

    auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(driver_->GetControllerCore());
    if (!controllerCore) return kIOReturnNotReady;

    auto* cmpClient = controllerCore->GetCMPClient();
    if (!cmpClient) return kIOReturnNotReady;

    constexpr uint8_t kTestPlug = 0;
    constexpr uint8_t kTestChannel = 0;  // Must match IRM-allocated channel

    ASFW_LOG(UserClient, "TestCMPConnectIPCR: Connecting iPCR[%u] ch=%u", kTestPlug, kTestChannel);

    cmpClient->ConnectIPCR(kTestPlug, kTestChannel,
        [](ASFW::CMP::CMPStatus status) {
            if (status == ASFW::CMP::CMPStatus::Success) {
                ASFW_LOG(UserClient, "✅ CMP iPCR connect succeeded!");
            } else {
                ASFW_LOG(UserClient, "❌ CMP iPCR connect failed: %d", static_cast<int>(status));
            }
        });

    return kIOReturnSuccess;
}

kern_return_t IsochHandler::TestCMPDisconnectIPCR(IOUserClientMethodArguments* args) {
    ASFW_LOG(UserClient, "TestCMPDisconnectIPCR called");

    auto* controllerCore = static_cast<ASFW::Driver::ControllerCore*>(driver_->GetControllerCore());
    if (!controllerCore) return kIOReturnNotReady;

    auto* cmpClient = controllerCore->GetCMPClient();
    if (!cmpClient) return kIOReturnNotReady;

    constexpr uint8_t kTestPlug = 0;
    ASFW_LOG(UserClient, "TestCMPDisconnectIPCR: Disconnecting iPCR[%u]", kTestPlug);

    cmpClient->DisconnectIPCR(kTestPlug,
        [](ASFW::CMP::CMPStatus status) {
            if (status == ASFW::CMP::CMPStatus::Success) {
                ASFW_LOG(UserClient, "✅ CMP iPCR disconnect succeeded!");
            } else {
                ASFW_LOG(UserClient, "❌ CMP iPCR disconnect failed: %d", static_cast<int>(status));
            }
        });

    return kIOReturnSuccess;
}

// ============================================================================
// Isoch Streaming Control
// ============================================================================

kern_return_t IsochHandler::StartIsochReceive(IOUserClientMethodArguments* args) {
    // Arguments: [0] = channel
    if (args->scalarInputCount < 1) return kIOReturnBadArgument;
    uint64_t channel = args->scalarInput[0];
    
    ASFW_LOG(UserClient, "StartIsochReceive called for channel %llu", channel);
    return driver_->StartIsochReceive(static_cast<uint8_t>(channel));
}

kern_return_t IsochHandler::StopIsochReceive(IOUserClientMethodArguments* args) {
     ASFW_LOG(UserClient, "StopIsochReceive called");
     return driver_->StopIsochReceive();
}

// ============================================================================
// Isoch Metrics
// ============================================================================

kern_return_t IsochHandler::GetIsochRxMetrics(IOUserClientMethodArguments* args) {
    ASFW_LOG_V3(UserClient, "GetIsochRxMetrics called");
    
    // Get the isoch receive context to fetch metrics
    auto* context = static_cast<ASFW::Isoch::IsochReceiveContext*>(driver_->GetIsochReceiveContext());
    if (!context) {
        ASFW_LOG_V3(UserClient, "GetIsochRxMetrics: No active context");
        // Return zeroed snapshot
        ASFW::Metrics::IsochRxSnapshot snapshot{};
        OSData* data = OSData::withBytes(&snapshot, sizeof(snapshot));
        if (!data) return kIOReturnNoMemory;
        args->structureOutput = data;
        return kIOReturnSuccess;
    }
    
    // Get StreamProcessor stats
    auto& processor = context->GetStreamProcessor();
    
    // Build snapshot
    ASFW::Metrics::IsochRxSnapshot snapshot{};
    snapshot.totalPackets = processor.PacketCount();
    snapshot.dataPackets = processor.SamplePacketCount();
    snapshot.emptyPackets = processor.EmptyPacketCount();
    snapshot.drops = processor.DiscontinuityCount();
    snapshot.errors = processor.ErrorCount();
    
    // Latency histogram
    snapshot.latencyHist[0] = processor.LatencyBucket0();
    snapshot.latencyHist[1] = processor.LatencyBucket1();
    snapshot.latencyHist[2] = processor.LatencyBucket2();
    snapshot.latencyHist[3] = processor.LatencyBucket3();
    
    snapshot.lastPollLatencyUs = processor.LastPollLatencyUs();
    snapshot.lastPollPackets = processor.LastPollPackets();
    
    // CIP info from processor
    snapshot.cipSID = processor.LastCipSID();
    snapshot.cipDBS = processor.LastCipDBS();
    snapshot.cipFDF = processor.LastCipFDF();
    snapshot.cipSYT = processor.LastSYT();
    snapshot.cipDBC = processor.LastDBC();
    
    OSData* data = OSData::withBytes(&snapshot, sizeof(snapshot));
    if (!data) return kIOReturnNoMemory;
    args->structureOutput = data;
    
    return kIOReturnSuccess;
}

kern_return_t IsochHandler::ResetIsochRxMetrics(IOUserClientMethodArguments* arguments) {
    if (!driver_) return kIOReturnNotReady;
    
    // Get context
    auto* context = static_cast<ASFW::Isoch::IsochReceiveContext*>(driver_->GetIsochReceiveContext());
    if (!context) {
        return kIOReturnNotReady;
    }
    
    ASFW_LOG(UserClient, "ResetIsochRxMetrics: resetting metrics");
    
    // Get StreamProcessor and reset
    context->GetStreamProcessor().Reset();
    
    return kIOReturnSuccess;
}

// ============================================================================
// IT Streaming Control
// ============================================================================

kern_return_t IsochHandler::StartIsochTransmit(IOUserClientMethodArguments* args) {
    // Arguments: [0] = channel (optional, default 0 - must match IRM allocation)
    // NOTE: Currently hardcoded to channel 0 to match IRM allocation.
    // TODO: Get channel from IRM allocation result for proper coordination.
    constexpr uint8_t channel = 0; // Must match IRM-allocated channel
    (void)args; // Ignore user argument for now - always use channel 0

    ASFW_LOG(UserClient, "StartIsochTransmit: Starting IT DMA on channel %u", channel);
    return driver_->StartIsochTransmit(channel);
}

kern_return_t IsochHandler::StopIsochTransmit(IOUserClientMethodArguments* args) {
    ASFW_LOG(UserClient, "StopIsochTransmit called");
    return driver_->StopIsochTransmit();
}

} // namespace ASFW::UserClient

