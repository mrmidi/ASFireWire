//
//  ASFWDriverUserClient.cpp
//  ASFWDriver
//
//  User client for GUI application communication
//  Refactored into handler-based architecture for maintainability
//

#include "ASFWDriverUserClient.h"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Shared/DriverVersionInfo.hpp"
#include "../../Version/DriverVersion.hpp"
#include "ASFWDriver.h"
#include "UserClientRuntimeState.hpp"
#include "../../Audio/Core/AudioCoordinator.hpp"
#include "../../Audio/Shared/Configuration/DeviceConfigurationSnapshot.hpp"
#include "../../Service/DriverContext.hpp"
#include "../WireFormats/AudioConfigurationWireFormats.hpp"
#include "../WireFormats/AudioTuningWireFormats.hpp"
#include "../WireFormats/AudioControlSurfaceWireFormats.hpp"
#include "../WireFormats/AudioMeterWireFormats.hpp"
#include "../WireFormats/AudioSemanticTopologyWireFormats.hpp"
#include "../WireFormats/AudioSemanticConsoleLayoutWireFormats.hpp"
#include "../WireFormats/AudioSemanticMatrixWireFormats.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include <memory>
#include <optional>

// Method selectors for ExternalMethod (matching .iig definitions)
enum {
    kMethodGetBusResetCount = 0,
    kMethodGetBusResetHistory = 1,
    kMethodGetControllerStatus = 2,
    kMethodGetMetricsSnapshot = 3,
    kMethodClearHistory = 4,
    kMethodGetSelfIDCapture = 5,
    // 6 (kMethodGetTopologySnapshot) retired: topology now served via the
    // diagnostics ABI (kMethodDiagGetTopology / ASFWDiagTopology).
    kMethodPing = 7,
    kMethodAsyncRead = 8,
    kMethodAsyncWrite = 9,
    kMethodRegisterStatusListener = 10,
    kMethodCopyStatusSnapshot = 11,
    kMethodGetTransactionResult = 12,
    kMethodRegisterTransactionListener = 13,
    kMethodExportConfigROM = 14,
    kMethodTriggerROMRead = 15,
    kMethodGetDiscoveredDevices = 16,
    kMethodAsyncCompareSwap = 17,
    kMethodGetDriverVersion = 18,
    kMethodSetAsyncVerbosity = 19,
    kMethodSetHexDumps = 20,
    kMethodGetLogConfig = 21,
    kMethodGetAVCUnits = 22,
    kMethodGetSubunitCapabilities = 23,
    kMethodGetSubunitDescriptor = 24,
    kMethodReScanAVCUnits = 25,
    kMethodSendRawFCPCommand = 38,
    kMethodGetRawFCPCommandResult = 39,
    kMethodSubmitSignalFormatProbe = 64,
    kMethodGetAudioConfiguration = 1015,
    kMethodRequestAudioConfiguration = 1016,
    kMethodGetAudioConfigurationEndpoints = 1017,
    kMethodGetAudioControlSurface = 1018,
    kMethodRequestAudioControlValue = 1019,
    kMethodGetAudioMeterSnapshot = 1020,
    kMethodSetAudioMeteringEnabled = 1021,
    kMethodSubmitAudioControlValue = 1022,
    kMethodGetAudioConfigurationAsync = 1023,
    kMethodGetAudioControlSurfaceAsync = 1024,
    kMethodGetAudioMeterSnapshotAsync = 1025,
    kMethodSetAudioMeteringEnabledAsync = 1026,
    kMethodRequestAudioConfigurationAsync = 1027,
    kMethodGetAudioRuntimeTuning = 1033,
    kMethodRequestAudioRuntimeTuning = 1034,
    kMethodGetAudioRuntimeTuningEndpoints = 1035,
    kMethodStartTxLatencySession = 1036,
    kMethodGetTxLatencyResults = 1037,
    kMethodGetAudioSemanticTopology = 1028,
    kMethodGetAudioSemanticTopologyEndpoints = 1029,
    kMethodGetAudioSemanticConsoleLayout = 1030,
    kMethodGetAudioSemanticMatrix = 1031,
    kMethodGetAudioSemanticMatrixEndpoints = 1032,
    kMethodSetIsochVerbosity = 40,
    // 41 retired (was the dev TX-verifier toggle)
    kMethodSetAudioAutoStart = 42,
    kMethodGetAudioAutoStart = 43,
    kMethodAsyncBlockRead = 44,
    kMethodAsyncBlockWrite = 45,
    // SBP2 address space management
    kMethodAllocateAddressRange = 46,
    kMethodDeallocateAddressRange = 47,
    kMethodReadIncomingData = 48,
    kMethodWriteLocalData = 49,
    kMethodCreateSBP2Session = 52,
    kMethodStartSBP2Login = 53,
    kMethodGetSBP2SessionState = 54,
    kMethodSubmitSBP2Inquiry = 55,
    kMethodGetSBP2InquiryResult = 56,
    kMethodSubmitSBP2Command = 57,
    kMethodGetSBP2CommandResult = 58,
    kMethodSubmitSBP2TaskManagement = 59,
    kMethodReleaseSBP2Session = 60,
    kMethodRequestUserBusReset = 61,
    kMethodStartAudioStreaming = 62,
    kMethodStopAudioStreaming = 63,
    // TODO(ASFW-IRM): Remove temporary IRM test method after dedicated validation tooling exists.
    kMethodTestIRMAllocation = 26,
    kMethodTestIRMRelease = 27,
    // TODO(ASFW-CMP): Remove temporary CMP test methods after dedicated validation tooling exists.
    kMethodTestCMPConnectOPCR = 28,
    kMethodTestCMPDisconnectOPCR = 29,
    kMethodTestCMPConnectIPCR = 30,
    kMethodTestCMPDisconnectIPCR = 31,

    // Isoch Stream Control
    kMethodStartIsochReceive = 32,
    kMethodStopIsochReceive = 33,

    // Isoch Metrics
    kMethodGetIsochRxMetrics = 34,
    kMethodResetIsochRxMetrics = 35,

    // Isoch Transmit Control (IT DMA allocation only - no CMP)
    kMethodStartIsochTransmit = 36,
    kMethodStopIsochTransmit = 37,

    // DV capture (raw DIF stream via shared ring, memory type 1)
    kMethodStartDVCapture = 50,
    kMethodStopDVCapture = 51,
};

namespace {

using MethodDispatchResult = std::optional<kern_return_t>;

std::optional<uint32_t> GetFirstScalarInput(const IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount < 1) {
        return std::nullopt;
    }

    return static_cast<uint32_t>(arguments->scalarInput[0]);
}

MethodDispatchResult DispatchBusResetMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                             IOUserClientMethodArguments* arguments,
                                             uint64_t selector) {
    switch (selector) {
    case kMethodGetBusResetCount:
        return runtimeState.BusReset().GetBusResetCount(arguments);
    case kMethodGetBusResetHistory:
        return runtimeState.BusReset().GetBusResetHistory(arguments);
    case kMethodClearHistory:
        return runtimeState.BusReset().ClearHistory(arguments);
    case kMethodRequestUserBusReset:
        return runtimeState.BusReset().RequestUserReset(arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchTopologyMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                             IOUserClientMethodArguments* arguments,
                                             uint64_t selector) {
    switch (selector) {
    case kMethodGetSelfIDCapture:
        return runtimeState.Topology().GetSelfIDCapture(arguments);
    // kMethodGetTopologySnapshot (6) retired: topology now served via the
    // diagnostics ABI (kMethodDiagGetTopology / ASFWDiagTopology).
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchStatusMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                           ASFWDriverUserClient& userClient,
                                           IOUserClientMethodArguments* arguments,
                                           uint64_t selector) {
    switch (selector) {
    case kMethodGetControllerStatus:
        return runtimeState.Status().GetControllerStatus(arguments);
    case kMethodGetMetricsSnapshot:
        return runtimeState.Status().GetMetricsSnapshot(arguments);
    case kMethodPing:
        return runtimeState.Status().Ping(arguments);
    case kMethodRegisterStatusListener:
        return runtimeState.Status().RegisterStatusListener(arguments, &userClient);
    case kMethodCopyStatusSnapshot:
        return runtimeState.Status().CopyStatusSnapshot(arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchTransactionMethods(
    ASFW::UserClient::UserClientRuntimeState& runtimeState, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments, uint64_t selector) {
    switch (selector) {
    case kMethodAsyncRead:
        return runtimeState.Transactions().AsyncRead(arguments, &userClient);
    case kMethodAsyncWrite:
        return runtimeState.Transactions().AsyncWrite(arguments, &userClient);
    case kMethodAsyncBlockRead:
        return runtimeState.Transactions().AsyncBlockRead(arguments, &userClient);
    case kMethodAsyncBlockWrite:
        return runtimeState.Transactions().AsyncBlockWrite(arguments, &userClient);
    case kMethodGetTransactionResult:
        return runtimeState.Transactions().GetTransactionResult(arguments);
    case kMethodRegisterTransactionListener:
        return runtimeState.Transactions().RegisterTransactionListener(arguments, &userClient);
    case kMethodAsyncCompareSwap:
        return runtimeState.Transactions().AsyncCompareSwap(arguments, &userClient);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchConfigRomMethods(
    ASFW::UserClient::UserClientRuntimeState& runtimeState, IOUserClientMethodArguments* arguments,
    uint64_t selector) {
    switch (selector) {
    case kMethodExportConfigROM:
        return runtimeState.ConfigROM().ExportConfigROM(arguments);
    case kMethodTriggerROMRead:
        return runtimeState.ConfigROM().TriggerROMRead(arguments);
    case kMethodGetDiscoveredDevices:
        return runtimeState.DeviceDiscovery().GetDiscoveredDevices(arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchAVCMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                        IOUserClientMethodArguments* arguments,
                                        uint64_t selector) {
    switch (selector) {
    case kMethodGetAVCUnits:
        return runtimeState.AVC().GetAVCUnits(arguments);
    case kMethodGetSubunitCapabilities:
        return runtimeState.AVC().GetSubunitCapabilities(arguments);
    case kMethodGetSubunitDescriptor:
        return runtimeState.AVC().GetSubunitDescriptor(arguments);
    case kMethodReScanAVCUnits:
        return runtimeState.AVC().ReScanAVCUnits(arguments);
    case kMethodSendRawFCPCommand:
        return runtimeState.AVC().SendRawFCPCommand(arguments);
    case kMethodGetRawFCPCommandResult:
        return runtimeState.AVC().GetRawFCPCommandResult(arguments);
    case kMethodSubmitSignalFormatProbe:
        return runtimeState.AVC().SubmitSignalFormatProbe(arguments);
    default:
        return std::nullopt;
    }
}

kern_return_t HandleGetDriverVersion(IOUserClientMethodArguments* arguments) {
    ASFW_LOG_V3(UserClient, "GetDriverVersion called");
    ASFW_LOG_V3(UserClient, "  structureOutput=%p", arguments->structureOutput);
    ASFW_LOG_V3(UserClient, "  structureOutputDescriptor=%p",
                arguments->structureOutputDescriptor);

    const ASFW::Shared::DriverVersionInfo versionInfo =
        ASFW::Shared::DriverVersionInfo::Create(ASFW::Version::kSemanticVersion,
                                                ASFW::Version::kGitCommitShort,
                                                ASFW::Version::kGitCommitFull,
                                                ASFW::Version::kGitBranch,
                                                ASFW::Version::kBuildTimestamp,
                                                ASFW::Version::kBuildHost,
                                                ASFW::Version::kGitDirty);

    ASFW_LOG_V3(UserClient, "  Creating OSData with %zu bytes", sizeof(versionInfo));

    OSData* data = OSData::withBytes(&versionInfo, sizeof(versionInfo));
    if (!data) {
        ASFW_LOG_V0(UserClient, "  OSData::withBytes failed!");
        return kIOReturnNoMemory;
    }

    arguments->structureOutput = data;
    ASFW_LOG_V3(UserClient, "GetDriverVersion: %{public}s", ASFW::Version::kFullVersionString);
    return kIOReturnSuccess;
}

MethodDispatchResult DispatchDriverScalarSetters(ASFWDriver& driver,
                                                 IOUserClientMethodArguments* arguments,
                                                 uint64_t selector) {
    const auto value = GetFirstScalarInput(arguments);
    switch (selector) {
    case kMethodSetAsyncVerbosity:
        return value ? MethodDispatchResult{driver.SetAsyncVerbosity(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    case kMethodSetIsochVerbosity:
        return value ? MethodDispatchResult{driver.SetIsochVerbosity(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    case kMethodSetHexDumps:
        return value ? MethodDispatchResult{driver.SetHexDumps(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    case kMethodSetAudioAutoStart:
        return value ? MethodDispatchResult{driver.SetAudioAutoStart(*value)}
                     : MethodDispatchResult{kIOReturnBadArgument};
    default:
        return std::nullopt;
    }
}

kern_return_t HandleGetAudioAutoStart(ASFWDriver& driver,
                                      IOUserClientMethodArguments* arguments) {
    if (!arguments->scalarOutput || arguments->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    uint32_t enabled = 0;
    const kern_return_t kr = driver.GetAudioAutoStart(&enabled);
    if (kr == kIOReturnSuccess) {
        arguments->scalarOutput[0] = enabled;
        arguments->scalarOutputCount = 1;
    }
    return kr;
}

kern_return_t HandleGetLogConfig(ASFWDriver& driver,
                                 IOUserClientMethodArguments* arguments) {
    if (!arguments->scalarOutput || arguments->scalarOutputCount < 2) {
        return kIOReturnBadArgument;
    }

    uint32_t asyncVerbosity = 0;
    uint32_t hexDumpsEnabled = 0;
    uint32_t isochVerbosity = 0;
    const kern_return_t kr =
        driver.GetLogConfig(&asyncVerbosity, &hexDumpsEnabled, &isochVerbosity);
    if (kr != kIOReturnSuccess) {
        return kr;
    }

    arguments->scalarOutput[0] = asyncVerbosity;
    arguments->scalarOutput[1] = hexDumpsEnabled;
    if (arguments->scalarOutputCount >= 4) {
        arguments->scalarOutput[2] = isochVerbosity;
        arguments->scalarOutput[3] = 0;  // reserved (was the removed dev TX-verifier flag)
        arguments->scalarOutputCount = 4;
    } else if (arguments->scalarOutputCount >= 3) {
        arguments->scalarOutput[2] = isochVerbosity;
        arguments->scalarOutputCount = 3;
    } else {
        arguments->scalarOutputCount = 2;
    }

    return kr;
}

kern_return_t HandleGetAudioConfiguration(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleRequestAudioConfiguration(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioConfigurationEndpoints(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioRuntimeTuningEndpoints(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioControlSurface(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioSemanticTopology(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioSemanticTopologyEndpoints(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioSemanticConsoleLayout(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioSemanticMatrix(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioSemanticMatrixEndpoints(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleRequestAudioControlValue(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioMeterSnapshot(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleSetAudioMeteringEnabled(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleSubmitAudioControlValue(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioConfigurationAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioControlSurfaceAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioMeterSnapshotAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments);
kern_return_t HandleSetAudioMeteringEnabledAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments);
kern_return_t HandleRequestAudioConfigurationAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments);
kern_return_t HandleGetAudioRuntimeTuning(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);
kern_return_t HandleRequestAudioRuntimeTuning(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments);

MethodDispatchResult DispatchDriverControlMethods(ASFWDriver& driver,
                                                  ASFWDriverUserClient& userClient,
                                                  IOUserClientMethodArguments* arguments,
                                                  uint64_t selector) {
    if (selector == kMethodGetDriverVersion) {
        return HandleGetDriverVersion(arguments);
    }

    if (const auto setterResult =
            DispatchDriverScalarSetters(driver, arguments, selector);
        setterResult.has_value()) {
        return setterResult;
    }

    switch (selector) {
    case kMethodStartAudioStreaming:
    case kMethodStopAudioStreaming: {
        const auto endpointId = GetFirstScalarInput(arguments);
        if (!endpointId.has_value() || *endpointId == 0) {
            return MethodDispatchResult{kIOReturnBadArgument};
        }
        return MethodDispatchResult{selector == kMethodStartAudioStreaming
                                        ? driver.StartAudioStreaming(*endpointId)
                                        : driver.StopAudioStreaming(*endpointId)};
    }
    case kMethodGetAudioAutoStart:
        return HandleGetAudioAutoStart(driver, arguments);
    case kMethodGetAudioConfiguration:
        return HandleGetAudioConfiguration(driver, arguments);
    case kMethodRequestAudioConfiguration:
        return HandleRequestAudioConfiguration(driver, arguments);
    case kMethodGetAudioRuntimeTuning:
        return HandleGetAudioRuntimeTuning(driver, arguments);
    case kMethodRequestAudioRuntimeTuning:
        return HandleRequestAudioRuntimeTuning(driver, arguments);
    case kMethodGetAudioConfigurationEndpoints:
        return HandleGetAudioConfigurationEndpoints(driver, arguments);
    case kMethodGetAudioRuntimeTuningEndpoints:
        return HandleGetAudioRuntimeTuningEndpoints(driver, arguments);
    case kMethodGetAudioControlSurface:
        return HandleGetAudioControlSurface(driver, arguments);
    case kMethodGetAudioSemanticTopology:
        return HandleGetAudioSemanticTopology(driver, arguments);
    case kMethodGetAudioSemanticTopologyEndpoints:
        return HandleGetAudioSemanticTopologyEndpoints(driver, arguments);
    case kMethodGetAudioSemanticConsoleLayout:
        return HandleGetAudioSemanticConsoleLayout(driver, arguments);
    case kMethodGetAudioSemanticMatrix:
        return HandleGetAudioSemanticMatrix(driver, arguments);
    case kMethodGetAudioSemanticMatrixEndpoints:
        return HandleGetAudioSemanticMatrixEndpoints(driver, arguments);
    case kMethodRequestAudioControlValue:
        return HandleRequestAudioControlValue(driver, arguments);
    case kMethodSubmitAudioControlValue:
        return HandleSubmitAudioControlValue(driver, userClient, arguments);
    case kMethodGetAudioConfigurationAsync:
        return HandleGetAudioConfigurationAsync(driver, userClient, arguments);
    case kMethodGetAudioControlSurfaceAsync:
        return HandleGetAudioControlSurfaceAsync(driver, userClient, arguments);
    case kMethodGetAudioMeterSnapshotAsync:
        return HandleGetAudioMeterSnapshotAsync(driver, userClient, arguments);
    case kMethodSetAudioMeteringEnabledAsync:
        return HandleSetAudioMeteringEnabledAsync(driver, userClient, arguments);
    case kMethodRequestAudioConfigurationAsync:
        return HandleRequestAudioConfigurationAsync(driver, userClient, arguments);
    case kMethodGetAudioMeterSnapshot:
        return HandleGetAudioMeterSnapshot(driver, arguments);
    case kMethodSetAudioMeteringEnabled:
        return HandleSetAudioMeteringEnabled(driver, arguments);
    case kMethodGetLogConfig:
        return HandleGetLogConfig(driver, arguments);
    default:
        return std::nullopt;
    }
}

MethodDispatchResult DispatchIsochMethods(ASFW::UserClient::UserClientRuntimeState& runtimeState,
                                          IOUserClientMethodArguments* arguments,
                                          uint64_t selector) {
    switch (selector) {
    case kMethodTestIRMAllocation:
        return runtimeState.Isoch().TestIRMAllocation(arguments);
    case kMethodTestIRMRelease:
        return runtimeState.Isoch().TestIRMRelease(arguments);
    case kMethodTestCMPConnectOPCR:
        return runtimeState.Isoch().TestCMPConnectOPCR(arguments);
    case kMethodTestCMPDisconnectOPCR:
        return runtimeState.Isoch().TestCMPDisconnectOPCR(arguments);
    case kMethodTestCMPConnectIPCR:
        return runtimeState.Isoch().TestCMPConnectIPCR(arguments);
    case kMethodTestCMPDisconnectIPCR:
        return runtimeState.Isoch().TestCMPDisconnectIPCR(arguments);
    case kMethodStartIsochReceive:
        return runtimeState.Isoch().StartIsochReceive(arguments);
    case kMethodStopIsochReceive:
        return runtimeState.Isoch().StopIsochReceive(arguments);
    case kMethodGetIsochRxMetrics:
        return runtimeState.Isoch().GetIsochRxMetrics(arguments);
    case kMethodResetIsochRxMetrics:
        return runtimeState.Isoch().ResetIsochRxMetrics(arguments);
    case kMethodStartIsochTransmit:
        return runtimeState.Isoch().StartIsochTransmit(arguments);
    case kMethodStopIsochTransmit:
        return runtimeState.Isoch().StopIsochTransmit(arguments);
    case kMethodStartDVCapture:
        return runtimeState.Isoch().StartDVCapture(arguments);
    case kMethodStopDVCapture:
        return runtimeState.Isoch().StopDVCapture(arguments);
    default:
        return std::nullopt;
    }
}

constexpr uint64_t kMethodDiagGetBusContract      = 1000;
constexpr uint64_t kMethodDiagGetTopology         = 1001;
constexpr uint64_t kMethodDiagGetRoleCoordinator  = 1002;
constexpr uint64_t kMethodDiagGetOHCI             = 1003;
constexpr uint64_t kMethodDiagGetPHY              = 1004;
constexpr uint64_t kMethodDiagGetCSRContract      = 1005;
constexpr uint64_t kMethodDiagGetAsyncTrace       = 1006;
constexpr uint64_t kMethodDiagGetInboundCSRStats  = 1007;
constexpr uint64_t kMethodDiagClearAsyncTrace     = 1008;
constexpr uint64_t kMethodDiagGetBusManager       = 1009;
constexpr uint64_t kMethodDiagGetPostResetTiming  = 1010;
constexpr uint64_t kMethodDiagGetLogRecords       = 1011;
constexpr uint64_t kMethodDiagGetLogStats         = 1012;
constexpr uint64_t kMethodDiagGetAudioTelemetry   = 1013;
constexpr uint64_t kMethodDiagGetLogCatalog       = 1014;
constexpr uint64_t kMethodDiagStartTxLatencySession = 1036;
constexpr uint64_t kMethodDiagGetTxLatencyResults   = 1037;

[[nodiscard]] uint8_t OpticalModeToWire(
    const std::optional<ASFW::Configuration::OpticalMode>& mode) noexcept {
    if (!mode) return 0;
    return *mode == ASFW::Configuration::OpticalMode::Adat ? 1U : 2U;
}

[[nodiscard]] uint64_t PackConfigurationCapability(
    const ASFW::Configuration::DeviceConfigurationCapabilitySnapshot& capability) noexcept {
    // [31:0] rate, [39:32] input channels, [47:40] output channels,
    // [55:48] input optical, [63:56] output optical.
    return static_cast<uint64_t>(capability.configuration.sampleRate) |
           (static_cast<uint64_t>(capability.inputChannels & 0xffU) << 32U) |
           (static_cast<uint64_t>(capability.outputChannels & 0xffU) << 40U) |
           (static_cast<uint64_t>(OpticalModeToWire(capability.configuration.opticalInput)) << 48U) |
           (static_cast<uint64_t>(OpticalModeToWire(capability.configuration.opticalOutput)) << 56U);
}

[[nodiscard]] uint64_t PackI16Quad(std::span<const int16_t> values, size_t offset) noexcept {
    uint64_t packed = 0;
    for (size_t index = 0; index < 4 && offset + index < values.size(); ++index) {
        packed |= static_cast<uint64_t>(static_cast<uint16_t>(values[offset + index])) <<
                  (index * 16U);
    }
    return packed;
}

void CompleteAudioControlPlaneAction(ASFWDriverUserClient* userClient,
                                     OSAction* completion,
                                     IOReturn status,
                                     const IOUserClientAsyncArgumentsArray& data,
                                     uint32_t dataCount) noexcept {
    userClient->AsyncCompletion(completion, status, data, dataCount);
}

kern_return_t HandleGetAudioConfiguration(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 1) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;
    ASFW::Configuration::DeviceConfigurationSnapshot snapshot{};
    const auto endpointId = ASFW::Audio::Devices::AudioEndpointId{
        arguments->scalarInput[0]};
    const kern_return_t kr = context->audioCoordinator->CopyDeviceConfigurationSnapshot(
        endpointId, snapshot);
    if (kr != kIOReturnSuccess) return kr;

    ASFW::UserClient::Wire::AudioConfigurationSnapshotWire wire{};
    wire.endpointId = snapshot.endpointId;
    wire.topologyRevision = snapshot.topologyRevision;
    wire.capabilityCount = snapshot.capabilityCount;
    const auto encode = [](const auto& source,
                           ASFW::UserClient::Wire::AudioConfigurationCapabilityWire& target) {
        target.sampleRateHz = source.configuration.sampleRate;
        target.inputChannels = source.inputChannels;
        target.outputChannels = source.outputChannels;
        target.opticalInput = OpticalModeToWire(source.configuration.opticalInput);
        target.opticalOutput = OpticalModeToWire(source.configuration.opticalOutput);
    };
    encode(ASFW::Configuration::DeviceConfigurationCapabilitySnapshot{
               .configuration = snapshot.committed,
               .inputChannels = snapshot.inputChannels,
               .outputChannels = snapshot.outputChannels,
           }, wire.committed);
    for (uint8_t i = 0; i < snapshot.capabilityCount; ++i) {
        encode(snapshot.capabilities[i], wire.capabilities[i]);
    }
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioConfigurationEndpoints(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || arguments->scalarInputCount != 0) return kIOReturnBadArgument;
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    std::array<ASFW::Audio::Devices::AudioEndpointId,
               ASFW::Configuration::kMaxConfigurationSnapshotCapabilities> endpointIds{};
    const uint32_t endpointCount =
        context->audioCoordinator->CopyConfigurationEndpointIds(endpointIds);

    ASFW::UserClient::Wire::AudioConfigurationEndpointListWire wire{};
    wire.endpointCount = endpointCount;
    for (uint32_t i = 0; i < endpointCount; ++i) {
        wire.endpointIds[i] = endpointIds[i].value;
    }
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

static_assert(ASFW::Audio::Shared::kMaxAudioRuntimeTuningEndpoints <=
                  ASFW::UserClient::Wire::kAudioConfigurationMaxCapabilities,
              "Runtime-tuning discovery must fit the endpoint-list reply it "
              "shares with configuration discovery");

kern_return_t HandleGetAudioRuntimeTuningEndpoints(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || arguments->scalarInputCount != 0) return kIOReturnBadArgument;
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    std::array<ASFW::Audio::Devices::AudioEndpointId,
               ASFW::Audio::Shared::kMaxAudioRuntimeTuningEndpoints> endpointIds{};
    const uint32_t endpointCount =
        context->audioCoordinator->CopyRuntimeTuningEndpointIds(endpointIds);

    // Same reply shape as the configuration list, so the app decodes both with
    // one decoder. Only the discovery predicate differs.
    ASFW::UserClient::Wire::AudioConfigurationEndpointListWire wire{};
    wire.endpointCount = endpointCount;
    for (uint32_t i = 0; i < endpointCount; ++i) {
        wire.endpointIds[i] = endpointIds[i].value;
    }
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleRequestAudioConfiguration(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 4) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;
    const uint32_t inputRaw = static_cast<uint32_t>(arguments->scalarInput[2]);
    const uint32_t outputRaw = static_cast<uint32_t>(arguments->scalarInput[3]);
    const auto decode = [](uint32_t raw)
        -> std::optional<ASFW::Configuration::OpticalMode> {
        switch (raw) {
        case 1: return ASFW::Configuration::OpticalMode::Adat;
        case 2: return ASFW::Configuration::OpticalMode::Spdif;
        default: return std::nullopt;
        }
    };
    const auto input = decode(inputRaw);
    const auto output = decode(outputRaw);
    if (!input || !output || arguments->scalarInput[1] == 0) {
        return kIOReturnBadArgument;
    }
    return context->audioCoordinator->RequestDeviceConfiguration(
        ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]},
        {.sampleRate = static_cast<uint32_t>(arguments->scalarInput[1]),
         .opticalInput = input,
         .opticalOutput = output});
}

kern_return_t HandleGetAudioRuntimeTuning(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 1) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    ASFW::UserClient::Wire::AudioTuningSnapshotWire wire{};
    const kern_return_t kr = context->audioCoordinator->CopyRuntimeTuningSnapshot(
        ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]}, wire);
    if (kr != kIOReturnSuccess) return kr;
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleRequestAudioRuntimeTuning(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->structureInput) return kIOReturnBadArgument;
    auto* input = OSDynamicCast(OSData, arguments->structureInput);
    if (!input || input->getLength() < sizeof(ASFW::UserClient::Wire::AudioTuningRequestWire)) {
        return kIOReturnBadArgument;
    }
    ASFW::UserClient::Wire::AudioTuningRequestWire request{};
    __builtin_memcpy(&request, input->getBytesNoCopy(), sizeof(request));
    if (request.version != ASFW::UserClient::Wire::kAudioTuningWireVersion) {
        return kIOReturnUnsupported;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    ASFW::Audio::Shared::AudioRuntimeTuning candidate{};
    candidate.txDispatchSlackPackets = request.txDispatchSlackPackets;
    candidate.txOwnershipGuardPackets = request.txOwnershipGuardPackets;
    candidate.outputLatencyFrames = request.outputLatencyFrames;
    candidate.inputLatencyFrames = request.inputLatencyFrames;
    candidate.outputSafetyOffsetFrames = request.outputSafetyOffsetFrames;
    candidate.inputSafetyOffsetFrames = request.inputSafetyOffsetFrames;
    candidate.frameRingFrames = request.frameRingFrames;
    candidate.clientIoBudgetFrames = request.clientIoBudgetFrames;
    candidate.zeroTimestampPeriodFrames = request.zeroTimestampPeriodFrames;
    return context->audioCoordinator->RequestRuntimeTuning(
        ASFW::Audio::Devices::AudioEndpointId{request.endpointId}, candidate,
        request.groups);
}

kern_return_t HandleGetAudioControlSurface(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 1) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    ASFW::Audio::AudioControlSurfaceSnapshot snapshot{};
    const auto endpointId = ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]};
    const kern_return_t kr = context->audioCoordinator->CopyAudioControlSurfaceSnapshot(
        endpointId, snapshot);
    if (kr != kIOReturnSuccess) return kr;

    ASFW::UserClient::Wire::AudioControlSurfaceSnapshotWire wire{};
    wire.kind = static_cast<uint32_t>(snapshot.kind);
    wire.endpointId = endpointId.value;
    wire.topologyRevision = snapshot.topologyRevision;
    wire.stateRevision = snapshot.stateRevision;
    wire.valueCount = snapshot.valueCount;
    for (uint32_t i = 0; i < snapshot.valueCount; ++i) {
        wire.values[i] = {.id = snapshot.values[i].id, .value = snapshot.values[i].value};
    }
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioSemanticTopology(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 1) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    const auto endpointId = ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]};
    ASFW::UserClient::Wire::AudioSemanticTopologySnapshotWire wire{};
    const kern_return_t kr = context->audioCoordinator->CopyAudioSemanticTopology(
        endpointId, wire.topology);
    if (kr != kIOReturnSuccess) return kr;
    wire.endpointId = endpointId.value;
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioSemanticTopologyEndpoints(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || arguments->scalarInputCount != 0) return kIOReturnBadArgument;
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    std::array<ASFW::Audio::Devices::AudioEndpointId,
               ASFW::Audio::kMaxAudioSemanticTopologyEndpoints> endpointIds{};
    const uint32_t endpointCount =
        context->audioCoordinator->CopySemanticTopologyEndpointIds(endpointIds);

    ASFW::UserClient::Wire::AudioSemanticTopologyEndpointListWire wire{};
    wire.endpointCount = endpointCount;
    for (uint32_t i = 0; i < endpointCount; ++i) {
        wire.endpointIds[i] = endpointIds[i].value;
    }
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioSemanticConsoleLayout(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 1) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    const auto endpointId = ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]};
    ASFW::UserClient::Wire::AudioSemanticConsoleLayoutSnapshotWire wire{};
    const kern_return_t kr = context->audioCoordinator->CopyAudioSemanticConsoleLayout(
        endpointId, wire.layout);
    if (kr != kIOReturnSuccess) return kr;
    wire.endpointId = endpointId.value;
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioSemanticMatrix(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 1) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    const auto endpointId = ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]};
    ASFW::UserClient::Wire::AudioSemanticMatrixSnapshotWire wire{};
    const kern_return_t kr = context->audioCoordinator->CopyAudioSemanticMatrix(
        endpointId, wire.matrix);
    if (kr != kIOReturnSuccess) return kr;
    wire.endpointId = endpointId.value;
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioSemanticMatrixEndpoints(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || arguments->scalarInputCount != 0) return kIOReturnBadArgument;
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    std::array<ASFW::Audio::Devices::AudioEndpointId,
               ASFW::Audio::kMaxAudioSemanticMatrixEndpoints> endpointIds{};
    const uint32_t endpointCount =
        context->audioCoordinator->CopySemanticMatrixEndpointIds(endpointIds);

    ASFW::UserClient::Wire::AudioSemanticMatrixEndpointListWire wire{};
    wire.endpointCount = endpointCount;
    for (uint32_t i = 0; i < endpointCount; ++i) {
        wire.endpointIds[i] = endpointIds[i].value;
    }
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleRequestAudioControlValue(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 3) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;
    return context->audioCoordinator->RequestAudioControlValue(
        ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]},
        static_cast<uint32_t>(arguments->scalarInput[1]),
        static_cast<int32_t>(arguments->scalarInput[2]));
}

kern_return_t HandleSubmitAudioControlValue(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments) {
    // endpoint, semantic control ID, value, client request ID. The completion
    // action exists only for IOConnectCallAsyncScalarMethod and must be held
    // until the device's async transaction has completed.
    if (!arguments || !arguments->completion || !arguments->scalarInput ||
        arguments->scalarInputCount != 4) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    const auto endpointId = ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]};
    const uint32_t controlId = static_cast<uint32_t>(arguments->scalarInput[1]);
    const int32_t value = static_cast<int32_t>(arguments->scalarInput[2]);
    const uint64_t requestId = arguments->scalarInput[3];
    OSAction* const completion = arguments->completion;
    completion->retain();
    userClient.retain();

    const kern_return_t started = context->audioCoordinator->SubmitAudioControlValue(
        endpointId, controlId, value,
        [&userClient, completion, requestId, controlId](IOReturn status) {
            IOUserClientAsyncArgumentsArray data{};
            data[0] = requestId;
            data[1] = controlId;
            userClient.AsyncCompletion(completion, status, data, 2);
            completion->release();
            userClient.release();
        });
    if (started != kIOReturnSuccess) {
        IOUserClientAsyncArgumentsArray data{};
        data[0] = requestId;
        data[1] = controlId;
        userClient.AsyncCompletion(completion, started, data, 2);
        completion->release();
        userClient.release();
        return kIOReturnSuccess;
    }
    return started;
}

kern_return_t HandleGetAudioConfigurationAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->completion || !arguments->scalarInput ||
        arguments->scalarInputCount != 1) {
        ASFW_LOG_ERROR(UserClient,
                       "[ControlPlane] configuration snapshot rejected: invalid async arguments");
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) {
        ASFW_LOG_ERROR(UserClient,
                       "[ControlPlane] configuration snapshot rejected: coordinator/scheduler unavailable");
        return kIOReturnNotReady;
    }
    const uint64_t requestId = arguments->scalarInput[0];
    // These are bounded, driver-owned snapshot copies only. They issue no
    // FireWire transaction and do not wait for hardware; completing them on
    // the UserClient queue avoids coupling UI liveness to the driver's general
    // scheduler (which may legitimately be occupied by transport work).
    IOUserClientAsyncArgumentsArray data{};
    data[0] = requestId;
    std::array<ASFW::Audio::Devices::AudioEndpointId,
               ASFW::Configuration::kMaxConfigurationSnapshotCapabilities> endpoints{};
    const uint32_t endpointCount = context->audioCoordinator->CopyConfigurationEndpointIds(endpoints);
    if (endpointCount == 0) {
        ASFW_LOG_ERROR(UserClient,
                       "[ControlPlane] configuration snapshot unavailable: no configurable endpoint");
        CompleteAudioControlPlaneAction(&userClient, arguments->completion, kIOReturnNotReady, data, 1);
        return kIOReturnSuccess;
    }
    // This is the M-Audio special-family control plane, not a generic
    // "first configurable device" UI.  A bus can publish multiple audio
    // endpoints, so choose only the endpoint whose semantic surface declares
    // the required family capability.
    ASFW::Audio::Devices::AudioEndpointId endpoint{};
    for (uint32_t index = 0; index < endpointCount; ++index) {
        ASFW::Audio::AudioControlSurfaceSnapshot surface{};
        if (endpoints[index].value != 0 &&
            context->audioCoordinator->CopyAudioControlSurfaceSnapshot(endpoints[index], surface) ==
                kIOReturnSuccess &&
            surface.kind == ASFW::Audio::AudioControlSurfaceKind::MAudio1814Mixer) {
            endpoint = endpoints[index];
            break;
        }
    }
    if (!endpoint) {
        ASFW_LOG_ERROR(UserClient,
                       "[ControlPlane] configuration snapshot unavailable: no M-Audio special surface");
        CompleteAudioControlPlaneAction(&userClient, arguments->completion, kIOReturnNoDevice, data, 1);
        return kIOReturnSuccess;
    }
    ASFW::Configuration::DeviceConfigurationSnapshot snapshot{};
    const kern_return_t status =
        context->audioCoordinator->CopyDeviceConfigurationSnapshot(endpoint, snapshot);
    if (status != kIOReturnSuccess) {
        ASFW_LOG_ERROR(UserClient,
                       "[ControlPlane] configuration snapshot failed endpoint=%llu kr=0x%x",
                       endpoint.value, static_cast<uint32_t>(status));
        CompleteAudioControlPlaneAction(&userClient, arguments->completion, status, data, 1);
        return kIOReturnSuccess;
    }
    data[1] = snapshot.endpointId;
    data[2] = snapshot.topologyRevision;
    data[3] = PackConfigurationCapability({
        .configuration = snapshot.committed,
        .inputChannels = snapshot.inputChannels,
        .outputChannels = snapshot.outputChannels,
    });
    data[4] = snapshot.capabilityCount;
    for (uint32_t index = 0; index < snapshot.capabilityCount; ++index) {
        data[5 + index] = PackConfigurationCapability(snapshot.capabilities[index]);
    }
    CompleteAudioControlPlaneAction(&userClient, arguments->completion, kIOReturnSuccess, data,
                                    5 + snapshot.capabilityCount);
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioControlSurfaceAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->completion || !arguments->scalarInput ||
        arguments->scalarInputCount != 2) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) {
        return kIOReturnNotReady;
    }
    const auto endpoint = ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]};
    const uint64_t requestId = arguments->scalarInput[1];
    IOUserClientAsyncArgumentsArray data{};
    data[0] = requestId;
    ASFW::Audio::AudioControlSurfaceSnapshot snapshot{};
    const kern_return_t status =
        context->audioCoordinator->CopyAudioControlSurfaceSnapshot(endpoint, snapshot);
    if (status != kIOReturnSuccess) {
        CompleteAudioControlPlaneAction(&userClient, arguments->completion, status, data, 1);
        return kIOReturnSuccess;
    }
    // Header only. An async completion carries at most
    // kIOUserClientAsyncArgumentsCountMax (16) scalars, which is nowhere near a
    // whole control surface — the 1814's is 78 values — and packing values here
    // would overrun `data`. This selector is therefore a *change notification*:
    // the client compares `stateRevision` and pulls the full surface with
    // kMethodGetAudioControlSurface, whose struct output has room for it.
    static_assert(5 <= kIOUserClientAsyncArgumentsCountMax,
                  "async control-surface header must fit the async argument array");
    data[1] = endpoint.value;
    data[2] = snapshot.topologyRevision;
    data[3] = static_cast<uint64_t>(snapshot.stateRevision) |
              (static_cast<uint64_t>(snapshot.kind) << 32U);
    data[4] = snapshot.valueCount;
    CompleteAudioControlPlaneAction(&userClient, arguments->completion, kIOReturnSuccess, data, 5);
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioMeterSnapshotAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->completion || !arguments->scalarInput ||
        arguments->scalarInputCount != 2) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) {
        return kIOReturnNotReady;
    }
    const auto endpoint = ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]};
    const uint64_t requestId = arguments->scalarInput[1];
    IOUserClientAsyncArgumentsArray data{};
    data[0] = requestId;
    ASFW::Audio::AudioMeterSnapshot snapshot{};
    const kern_return_t status = context->audioCoordinator->CopyAudioMeterSnapshot(endpoint, snapshot);
    if (status != kIOReturnSuccess) {
        CompleteAudioControlPlaneAction(&userClient, arguments->completion, status, data, 1);
        return kIOReturnSuccess;
    }
    data[1] = endpoint.value;
    data[2] = snapshot.topologyRevision;
    data[3] = static_cast<uint64_t>(snapshot.telemetrySequence) |
              (static_cast<uint64_t>(snapshot.detectedSampleRateHz) << 32U);
    data[4] = static_cast<uint64_t>(snapshot.valueCount) |
              (static_cast<uint64_t>(snapshot.enabled ? 1U : 0U) << 32U) |
              (static_cast<uint64_t>(snapshot.clockLocked ? 1U : 0U) << 33U) |
              (static_cast<uint64_t>(snapshot.externalSync ? 1U : 0U) << 34U) |
              (static_cast<uint64_t>(snapshot.hardwareSwitch ? 1U : 0U) << 35U) |
              (static_cast<uint64_t>(snapshot.rotaryCount & 0xFFU) << 36U);
    // Peaks pack four to a scalar, then one scalar carries the encoders. The
    // whole reply must stay inside kIOUserClientAsyncArgumentsCountMax.
    static_assert(5 + (ASFW::Audio::kMaxAudioMeterValues + 3) / 4 + 1 <=
                      kIOUserClientAsyncArgumentsCountMax,
                  "async meter reply no longer fits the async argument array");
    uint32_t used = 5;
    for (uint32_t index = 0; index < snapshot.valueCount; index += 4) {
        data[used++] = PackI16Quad(
            std::span<const int16_t>{snapshot.values.data(), snapshot.valueCount}, index);
    }
    data[used++] = PackI16Quad(
        std::span<const int16_t>{snapshot.rotaries.data(), snapshot.rotaries.size()}, 0);
    CompleteAudioControlPlaneAction(&userClient, arguments->completion, kIOReturnSuccess, data,
                                    used);
    return kIOReturnSuccess;
}

kern_return_t HandleSetAudioMeteringEnabledAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->completion || !arguments->scalarInput ||
        arguments->scalarInputCount != 3 || arguments->scalarInput[1] > 1U) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    const kern_return_t status = context->audioCoordinator->SetAudioMeteringEnabled(
        ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]},
        arguments->scalarInput[1] != 0U);
    IOUserClientAsyncArgumentsArray data{};
    data[0] = arguments->scalarInput[2]; // client request ID
    userClient.AsyncCompletion(arguments->completion, status, data, 1);
    return kIOReturnSuccess;
}

kern_return_t HandleRequestAudioConfigurationAsync(
    ASFWDriver& driver, ASFWDriverUserClient& userClient,
    IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->completion || !arguments->scalarInput ||
        arguments->scalarInputCount != 5) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    const auto decode = [](uint32_t raw)
        -> std::optional<ASFW::Configuration::OpticalMode> {
        switch (raw) {
        case 1: return ASFW::Configuration::OpticalMode::Adat;
        case 2: return ASFW::Configuration::OpticalMode::Spdif;
        default: return std::nullopt;
        }
    };
    const auto input = decode(static_cast<uint32_t>(arguments->scalarInput[2]));
    const auto output = decode(static_cast<uint32_t>(arguments->scalarInput[3]));
    const kern_return_t status = (!input || !output || arguments->scalarInput[1] == 0)
        ? kIOReturnBadArgument
        : context->audioCoordinator->RequestDeviceConfiguration(
            ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]},
            {.sampleRate = static_cast<uint32_t>(arguments->scalarInput[1]),
             .opticalInput = input,
             .opticalOutput = output});
    IOUserClientAsyncArgumentsArray data{};
    data[0] = arguments->scalarInput[4]; // client request ID
    userClient.AsyncCompletion(arguments->completion, status, data, 1);
    return kIOReturnSuccess;
}

kern_return_t HandleGetAudioMeterSnapshot(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 1) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;

    ASFW::Audio::AudioMeterSnapshot snapshot{};
    const auto endpointId = ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]};
    const kern_return_t kr = context->audioCoordinator->CopyAudioMeterSnapshot(endpointId,
                                                                                snapshot);
    if (kr != kIOReturnSuccess) return kr;
    ASFW::UserClient::Wire::AudioMeterSnapshotWire wire{};
    wire.endpointId = endpointId.value;
    wire.topologyRevision = snapshot.topologyRevision;
    wire.telemetrySequence = snapshot.telemetrySequence;
    wire.valueCount = snapshot.valueCount;
    wire.detectedSampleRateHz = snapshot.detectedSampleRateHz;
    wire.enabled = snapshot.enabled ? 1U : 0U;
    wire.clockLocked = snapshot.clockLocked ? 1U : 0U;
    wire.externalSync = snapshot.externalSync ? 1U : 0U;
    wire.hardwareSwitch = snapshot.hardwareSwitch ? 1U : 0U;
    wire.rotaryCount = snapshot.rotaryCount;
    for (uint32_t i = 0; i < snapshot.rotaryCount && i < wire.rotaries.size(); ++i) {
        wire.rotaries[i] = snapshot.rotaries[i];
    }
    for (uint32_t i = 0; i < snapshot.valueCount; ++i) wire.values[i] = snapshot.values[i];
    auto* data = OSData::withBytes(&wire, sizeof(wire));
    if (!data) return kIOReturnNoMemory;
    arguments->structureOutput = data;
    arguments->structureOutputDescriptor = nullptr;
    return kIOReturnSuccess;
}

kern_return_t HandleSetAudioMeteringEnabled(
    ASFWDriver& driver, IOUserClientMethodArguments* arguments) {
    if (!arguments || !arguments->scalarInput || arguments->scalarInputCount != 2 ||
        arguments->scalarInput[1] > 1U) {
        return kIOReturnBadArgument;
    }
    auto* context = static_cast<ServiceContext*>(driver.GetServiceContext());
    if (!context || !context->audioCoordinator) return kIOReturnNotReady;
    return context->audioCoordinator->SetAudioMeteringEnabled(
        ASFW::Audio::Devices::AudioEndpointId{arguments->scalarInput[0]},
        arguments->scalarInput[1] != 0U);
}

MethodDispatchResult DispatchDiagnosticsMethods(
    ASFW::UserClient::UserClientRuntimeState& runtimeState,
    IOUserClientMethodArguments* arguments, uint64_t selector) {
    switch (selector) {
    case kMethodDiagGetBusContract:
        return runtimeState.Diagnostics().GetBusContract(arguments);
    case kMethodDiagGetTopology:
        return runtimeState.Diagnostics().GetTopology(arguments);
    case kMethodDiagGetRoleCoordinator:
        return runtimeState.Diagnostics().GetRoleCoordinator(arguments);
    case kMethodDiagGetOHCI:
        return runtimeState.Diagnostics().GetOHCI(arguments);
    case kMethodDiagGetPHY:
        return runtimeState.Diagnostics().GetPHY(arguments);
    case kMethodDiagGetCSRContract:
        return runtimeState.Diagnostics().GetCSRContract(arguments);
    case kMethodDiagGetAsyncTrace:
        return runtimeState.Diagnostics().GetAsyncTrace(arguments);
    case kMethodDiagGetInboundCSRStats:
        return runtimeState.Diagnostics().GetInboundCSRStats(arguments);
    case kMethodDiagClearAsyncTrace:
        return runtimeState.Diagnostics().ClearAsyncTrace(arguments);
    case kMethodDiagGetBusManager:
        return runtimeState.Diagnostics().GetBusManager(arguments);
    case kMethodDiagGetPostResetTiming:
        return runtimeState.Diagnostics().GetPostResetTiming(arguments);
    case kMethodDiagGetLogRecords:
        return runtimeState.Diagnostics().GetLogRecords(arguments);
    case kMethodDiagGetLogStats:
        return runtimeState.Diagnostics().GetLogStats(arguments);
    case kMethodDiagGetAudioTelemetry:
        return runtimeState.Diagnostics().GetAudioTelemetry(arguments);
    case kMethodDiagGetLogCatalog:
        return runtimeState.Diagnostics().GetLogCatalog(arguments);
    case kMethodDiagStartTxLatencySession:
        return runtimeState.Diagnostics().StartTxLatencySession(arguments);
    case kMethodDiagGetTxLatencyResults:
        return runtimeState.Diagnostics().GetTxLatencyResults(arguments);
    default:
        return std::nullopt;
    }
}

} // namespace

bool ASFWDriverUserClient::init() {
    if (!super::init()) {
        return false;
    }

    ivars = IONewZero(ASFWDriverUserClient_IVars, 1);
    if (!ivars) {
        return false;
    }

    ivars->statusRegistered = false;
    ivars->statusAction = nullptr;
    ivars->transactionListenerRegistered = false;
    ivars->transactionAction = nullptr;
    ivars->actionLock = IOLockAlloc();
    if (!ivars->actionLock) {
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
        return false;
    }
    ivars->stopping = false;

    auto runtimeState = std::make_unique<ASFW::UserClient::UserClientRuntimeState>();
    if (!runtimeState || !runtimeState->IsValid()) {
        if (ivars->actionLock) {
            IOLockFree(ivars->actionLock);
            ivars->actionLock = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
        return false;
    }
    ivars->runtimeState = runtimeState.release();

    return true;
}

void ASFWDriverUserClient::free() {
    if (ivars) {
        if (ivars->driver && ivars->statusRegistered) {
            ivars->driver->UnregisterStatusListener(this);
        }
        if (ivars->actionLock) {
            IOLockLock(ivars->actionLock);
            ivars->stopping = true;
            if (ivars->statusAction) {
                ivars->statusAction->release();
                ivars->statusAction = nullptr;
            }
            if (ivars->transactionAction) {
                ivars->transactionAction->release();
                ivars->transactionAction = nullptr;
            }
            IOLockUnlock(ivars->actionLock);
            IOLockFree(ivars->actionLock);
            ivars->actionLock = nullptr;
        }

        if (ivars->runtimeState) {
            auto runtimeState =
                std::unique_ptr<ASFW::UserClient::UserClientRuntimeState>(
                    static_cast<ASFW::UserClient::UserClientRuntimeState*>(ivars->runtimeState));
            runtimeState->ReleaseOwner(this);
            ivars->runtimeState = nullptr;
        }
        IOSafeDeleteNULL(ivars, ASFWDriverUserClient_IVars, 1);
    }
    super::free();
}

kern_return_t IMPL(ASFWDriverUserClient, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // Store typed reference to driver
    ivars->driver = OSDynamicCast(ASFWDriver, provider);
    if (!ivars->driver) {
        return kIOReturnError;
    }

    if (ivars->actionLock) {
        IOLockLock(ivars->actionLock);
        ivars->stopping = false;
        IOLockUnlock(ivars->actionLock);
    }

    ivars->statusRegistered = false;
    if (ivars->statusAction) {
        ivars->statusAction->release();
        ivars->statusAction = nullptr;
    }

    auto* runtimeState = ASFW::UserClient::GetRuntimeState(this);
    if (!runtimeState || !runtimeState->BindDriver(ivars->driver, this)) {
        ASFW_LOG(UserClient, "Start() failed to initialize runtime state");
        return kIOReturnNoMemory;
    }

    ASFW_LOG(UserClient, "Start() completed - runtime state initialized");
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWDriverUserClient, Stop) {
    if (ivars && ivars->actionLock) {
        IOLockLock(ivars->actionLock);
        ivars->stopping = true;
        ivars->statusRegistered = false;
        ivars->transactionListenerRegistered = false;
        if (ivars->statusAction) {
            ivars->statusAction->release();
            ivars->statusAction = nullptr;
        }
        if (ivars->transactionAction) {
            ivars->transactionAction->release();
            ivars->transactionAction = nullptr;
        }
        IOLockUnlock(ivars->actionLock);
    }

    if (ivars && ivars->driver) {
        ivars->driver->UnregisterStatusListener(this);
        ivars->driver = nullptr;
    }
    if (auto* runtimeState = ASFW::UserClient::GetRuntimeState(this); runtimeState != nullptr) {
        runtimeState->ReleaseOwner(this);
        runtimeState->ResetHandlers();
    }

    ASFW_LOG(UserClient, "Stop() completed");
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t ASFWDriverUserClient::ExternalMethod(uint64_t selector,
                                                   IOUserClientMethodArguments* arguments,
                                                   const IOUserClientMethodDispatch* dispatch,
                                                   OSObject* target, void* reference) {
    (void)dispatch;
    (void)target;
    (void)reference;

    ASFW_LOG_V3(UserClient, "ExternalMethod called: selector=%llu", selector);

    if (!ivars || !ivars->driver) {
        ASFW_LOG(UserClient, "ExternalMethod: Not ready (ivars=%p driver=%p)", ivars,
                 ivars ? ivars->driver : nullptr);
        return kIOReturnNotReady;
    }

    auto* runtimeState = ASFW::UserClient::GetRuntimeState(this);
    if (runtimeState == nullptr || !runtimeState->HandlersReady()) {
        return kIOReturnNotReady;
    }

    if (auto result = DispatchBusResetMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchTopologyMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchStatusMethods(*runtimeState, *this, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchTransactionMethods(*runtimeState, *this, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchConfigRomMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchAVCMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchDriverControlMethods(*ivars->driver, *this, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchIsochMethods(*runtimeState, arguments, selector)) {
        return *result;
    }
    if (auto result = DispatchDiagnosticsMethods(*runtimeState, arguments, selector)) {
        return *result;
    }

    // main's SBP2 address-space management methods (46-49), wired into DICE's
    // dispatch-helper ExternalMethod.
    switch (selector) {
    case kMethodAllocateAddressRange:
        return runtimeState->SBP2().AllocateAddressRange(arguments, this);
    case kMethodDeallocateAddressRange:
        return runtimeState->SBP2().DeallocateAddressRange(arguments, this);
    case kMethodReadIncomingData:
        return runtimeState->SBP2().ReadIncomingData(arguments, this);
    case kMethodWriteLocalData:
        return runtimeState->SBP2().WriteLocalData(arguments, this);
    case kMethodCreateSBP2Session:
        return runtimeState->SBP2().CreateSBP2Session(arguments, this);
    case kMethodStartSBP2Login:
        return runtimeState->SBP2().StartSBP2Login(arguments, this);
    case kMethodGetSBP2SessionState:
        return runtimeState->SBP2().GetSBP2SessionState(arguments, this);
    case kMethodSubmitSBP2Inquiry:
        return runtimeState->SBP2().SubmitSBP2Inquiry(arguments, this);
    case kMethodGetSBP2InquiryResult:
        return runtimeState->SBP2().GetSBP2InquiryResult(arguments, this);
    case kMethodSubmitSBP2Command:
        return runtimeState->SBP2().SubmitSBP2Command(arguments, this);
    case kMethodGetSBP2CommandResult:
        return runtimeState->SBP2().GetSBP2CommandResult(arguments, this);
    case kMethodSubmitSBP2TaskManagement:
        return runtimeState->SBP2().SubmitSBP2TaskManagement(arguments, this);
    case kMethodReleaseSBP2Session:
        return runtimeState->SBP2().ReleaseSBP2Session(arguments, this);
    default:
        break;
    }

    return kIOReturnBadArgument;
}

// LOCALONLY user-client ABI entry point.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriverUserClient::AsyncRead(uint64_t deviceInstanceID, uint16_t addressHi,
                                              uint32_t addressLo, uint32_t length,
                                              uint16_t* handle) {
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 8
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    return kIOReturnUnsupported;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriverUserClient::AsyncWrite(uint64_t deviceInstanceID, uint16_t addressHi,
                                               uint32_t addressLo, uint32_t length,
                                               const uint8_t* payload, uint16_t* handle) {
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 9
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    return kIOReturnUnsupported;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriverUserClient::AsyncCompareSwap(uint64_t deviceInstanceID, uint16_t addressHi,
                                                     uint32_t addressLo, uint8_t size,
                                                     const uint8_t* compareValue, const uint8_t* newValue, // NOLINT(bugprone-easily-swappable-parameters)
                                                     uint16_t* handle, uint8_t* locked) {
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 17
    // This should never be called directly
    if (handle) {
        *handle = 0;
    }
    if (locked) {
        *locked = 0;
    }
    return kIOReturnUnsupported;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void ASFWDriverUserClient::NotifyStatus(uint64_t sequence, uint32_t reason) {
    if (!ivars || !ivars->actionLock) {
        return;
    }

    OSAction* action = nullptr;
    IOLockLock(ivars->actionLock);
    if (!ivars->stopping && ivars->statusRegistered && ivars->statusAction) {
        action = ivars->statusAction;
        action->retain();
    }
    IOLockUnlock(ivars->actionLock);

    if (!action) {
        return;
    }

    IOUserClientAsyncArgumentsArray data{};
    data[0] = sequence;
    data[1] = reason;
    AsyncCompletion(action, kIOReturnSuccess, data, 2);
    action->release();
}

void ASFWDriverUserClient::NotifyTransactionComplete(uint16_t handle, uint32_t status) {
    if (!ivars || !ivars->actionLock) {
        return;
    }

    ASFW_LOG(UserClient, "NotifyTransactionComplete: handle=0x%04x status=0x%08x", handle, status);

    OSAction* action = nullptr;
    IOLockLock(ivars->actionLock);
    if (!ivars->stopping && ivars->transactionListenerRegistered && ivars->transactionAction) {
        action = ivars->transactionAction;
        action->retain();
    }
    IOLockUnlock(ivars->actionLock);

    if (!action) {
        return;
    }

    IOUserClientAsyncArgumentsArray data{};
    data[0] = handle;
    data[1] = status;
    AsyncCompletion(action, kIOReturnSuccess, data, 2);
    action->release();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
kern_return_t ASFWDriverUserClient::GetTransactionResult(uint16_t handle, uint32_t* status,
                                                         uint32_t* dataLength, uint8_t* data,
                                                         uint32_t maxDataLength) {
    // LOCALONLY method - implementation is in TransactionHandler via ExternalMethod case 12
    // This should never be called directly
    if (status)
        *status = 0;
    if (dataLength)
        *dataLength = 0;
    return kIOReturnUnsupported;
}

kern_return_t IMPL(ASFWDriverUserClient, CopyClientMemoryForType) {
    if (!memory) {
        return kIOReturnBadArgument;
    }

    if (!ivars || !ivars->driver) {
        return kIOReturnNotReady;
    }

    // Type 0: shared status memory. Type 1: DV capture ring.
    if (type == 0) {
        return ivars->driver->CopySharedStatusMemory(options, memory);
    }
    if (type == 1) {
        const uint64_t ownerToken = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(this));
        return ivars->driver->CopyDVCaptureMemory(
            ownerToken, options, memory);
    }
    return kIOReturnUnsupported;
}

// Note: GetDiscoveredDevices is handled in ExternalMethod (selector 16), no stub needed
