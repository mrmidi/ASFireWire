//
//  TransactionHandler.cpp
//  ASFWDriver
//
//  Handler for async transaction related UserClient methods
//

#include "TransactionHandler.hpp"
#include "../../Audio/Families/BeBoB/Bootloader/BeBoBBootloaderCue.hpp"
#include "../../Controller/ControllerCore.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Logging/Logging.hpp"
#include "../Core/UserClientRuntimeState.hpp"
#include "../Storage/TransactionStorage.hpp"
#include "ASFWDriver.h"
#include "ASFWDriverUserClient.h"
#include "AsyncPortAccess.hpp"
#include "ControllerCoreAccess.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>

#include <optional>

namespace ASFW::UserClient {

namespace {

/// Refuses any user-client write into the BridgeCo bootloader register window.
///
/// That window is where a 12-byte write starts the application firmware — and
/// where the *same* write with one byte changed permanently reprograms device
/// identity in flash: 0x0a rewrites the EUI-64 GUID, 0x10 the hardware ID, and
/// 0x04-0x06 the firmware image. None of that is recoverable.
///
/// The driver's own cue path is separately interlocked (a closed value type with
/// no opcode parameter, plus IsPermittedBootloaderWrite at the transmit point),
/// but every one of those interlocks lives *inside* the bootloader client. This
/// path does not go through it: AsyncWrite/AsyncBlockWrite/AsyncCompareSwap take
/// an arbitrary caller-supplied address, so the MCP write tools and any other
/// user-client caller could otherwise reach that window directly.
///
/// The rule here is deliberately stricter than the client's: no user-client
/// write to this window is permitted at all, not even a well-formed cue. The
/// driver owns firmware preparation and decides it from catalog policy; there is
/// no legitimate external reason to write here.
[[nodiscard]] bool RefusesBootloaderWindow(uint16_t addressHi, uint32_t addressLo,
                                           const char* operation) {
    namespace Boot = ASFW::Audio::Families::BeBoB::Bootloader;
    if (!Boot::IsBootloaderWindow(addressHi, addressLo)) {
        return false;
    }
    ASFW_LOG_ERROR(UserClient,
                   "%{public}s: refused write to the BeBoB bootloader window "
                   "0x%04x:%08x - this range permanently reprograms device "
                   "identity in flash and is driver-owned",
                   operation, addressHi, addressLo);
    return true;
}

std::optional<ASFW::Discovery::DeviceRouteToken>
ResolveDeviceRoute(ASFWDriver* driver, uint64_t rawInstanceId, const char* operation) {
    const ASFW::Discovery::DeviceInstanceId instanceId{rawInstanceId};
    if (!instanceId) {
        ASFW_LOG(UserClient, "%{public}s: zero DeviceInstanceId", operation);
        return std::nullopt;
    }

    auto* controller = GetControllerCorePtr(driver);
    auto* registry = controller != nullptr ? controller->GetDeviceRegistry() : nullptr;
    if (registry == nullptr) {
        ASFW_LOG(UserClient, "%{public}s: DeviceRegistry unavailable", operation);
        return std::nullopt;
    }

    const auto route = registry->CurrentRoute(instanceId);
    if (!route.has_value()) {
        ASFW_LOG(UserClient,
                 "%{public}s: instance=%llu has no routable current binding "
                 "(missing, suspended, retired, or quarantined)",
                 operation, instanceId.value);
    }
    return route;
}

bool IsRouteCurrent(ASFWDriverUserClient* userClient,
                    const ASFW::Discovery::DeviceRouteToken& route) noexcept {
    if (userClient == nullptr || userClient->ivars == nullptr ||
        userClient->ivars->driver == nullptr) {
        return false;
    }
    auto* controller = GetControllerCorePtr(userClient->ivars->driver);
    auto* registry = controller != nullptr ? controller->GetDeviceRegistry() : nullptr;
    return registry != nullptr && registry->IsCurrent(route);
}

} // namespace

TransactionHandler::TransactionHandler(ASFWDriver* driver, TransactionStorage* storage)
    : driver_(driver), storage_(storage) {}

// Callback signature is fixed by the async subsystem completion contract.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void TransactionHandler::AsyncCompletionCallback(ASFW::Async::AsyncHandle handle,
                                                 ASFW::Async::AsyncStatus status,
                                                 uint8_t responseCode, void* context, // NOLINT(bugprone-easily-swappable-parameters)
                                                 ASFW::Discovery::DeviceRouteToken route,
                                                 const uint8_t* responsePayload,
                                                 uint32_t responseLength) {

    auto* userClient = static_cast<ASFWDriverUserClient*>(context);
    auto* runtimeState = GetRuntimeState(userClient);
    if (runtimeState == nullptr) {
        return;
    }

    // A successful packet response is not a successful user operation if the
    // physical route was invalidated while it was in flight. Fail closed so a
    // pre-reset request can never be attributed to the post-reset occupant of
    // the same node ID.
    if (!IsRouteCurrent(userClient, route)) {
        status = ASFW::Async::AsyncStatus::kStaleGeneration;
        responsePayload = nullptr;
        responseLength = 0;
    }

    // Store result in ring buffer
    runtimeState->TransactionResults().StoreResult(handle.value, static_cast<uint32_t>(status),
                                                   responseCode, responsePayload, responseLength);

    // Send async notification to GUI
    userClient->NotifyTransactionComplete(handle.value, static_cast<uint32_t>(status));

    ASFW_LOG(UserClient,
             "AsyncTransactionCompletion: handle=0x%04x status=%{public}s rCode=0x%02x len=%u stored",
             handle.value, ASFW::Async::ToString(status), responseCode, responseLength);
}

kern_return_t TransactionHandler::AsyncRead(IOUserClientMethodArguments* args,
                                            ASFWDriverUserClient* userClient) {
    // Input: DeviceInstanceId[64], addressHi[16], addressLo[32], length[32]
    // Output: handle[16]
    if (!args || args->scalarInputCount < 4 || args->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    const auto route = ResolveDeviceRoute(driver_, args->scalarInput[0], "AsyncRead");
    if (!route.has_value()) {
        return kIOReturnNotReady;
    }
    const uint16_t addressHi = static_cast<uint16_t>(args->scalarInput[1] & 0xFFFF);
    const uint32_t addressLo = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFFFFFFu);
    const uint32_t length = static_cast<uint32_t>(args->scalarInput[3] & 0xFFFFFFFFu);

    ASFW_LOG(UserClient,
             "AsyncRead: instance=%llu epoch=%llu node=%u addr=0x%04x:%08x len=%u",
             route->deviceInstanceId.value, route->routeEpoch, route->nodeId, addressHi,
             addressLo, length);

    using namespace ASFW::Async;
    auto* asyncPort = GetAsyncSubsystemPort(driver_);
    if (!asyncPort) {
        ASFW_LOG(UserClient, "AsyncRead: Async port not available");
        return kIOReturnNotReady;
    }

    // Build ReadParams
    ReadParams params{};
    params.destinationID = route->nodeId;
    params.addressHigh = addressHi;
    params.addressLow = addressLo;
    params.length = length;

    // Initiate async read with completion callback
    userClient->retain();
    AsyncHandle handle = asyncPort->Read(
        params, [userClient, route = *route](AsyncHandle handle, AsyncStatus status, uint8_t responseCode,
                             std::span<const uint8_t> responsePayload) {
            AsyncCompletionCallback(handle, status, responseCode, userClient, route,
                                    responsePayload.data(),
                                    static_cast<uint32_t>(responsePayload.size()));
            userClient->release();
        });
    if (!handle) {
        userClient->release();
        ASFW_LOG(UserClient, "AsyncRead: Failed to initiate transaction");
        return kIOReturnError;
    }

    args->scalarOutput[0] = handle.value;
    args->scalarOutputCount = 1;

    ASFW_LOG(UserClient, "AsyncRead: Initiated with handle=0x%04x (with completion callback)",
             handle.value);
    return kIOReturnSuccess;
}

kern_return_t TransactionHandler::AsyncWrite(IOUserClientMethodArguments* args,
                                             ASFWDriverUserClient* userClient) {
    // Input: DeviceInstanceId[64], addressHi[16], addressLo[32], length[32]
    // structureInput: payload data
    // Output: handle[16]
    if (!args || args->scalarInputCount < 4 || args->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    if (!args->structureInput) {
        ASFW_LOG(UserClient, "AsyncWrite: No payload data provided");
        return kIOReturnBadArgument;
    }

    // Get payload from structureInput (OSData) early to validate
    OSData* payloadData = OSDynamicCast(OSData, args->structureInput);
    if (!payloadData) {
        ASFW_LOG(UserClient, "AsyncWrite: structureInput is not OSData");
        return kIOReturnBadArgument;
    }

    const uint32_t actualPayloadSize = static_cast<uint32_t>(payloadData->getLength());
    if (actualPayloadSize == 0) {
        ASFW_LOG(UserClient, "AsyncWrite: Empty payload");
        return kIOReturnBadArgument;
    }

    const auto route = ResolveDeviceRoute(driver_, args->scalarInput[0], "AsyncWrite");
    if (!route.has_value()) {
        return kIOReturnNotReady;
    }
    const uint16_t addressHi = static_cast<uint16_t>(args->scalarInput[1] & 0xFFFF);
    const uint32_t addressLo = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFFFFFFu);
    const uint32_t length = static_cast<uint32_t>(args->scalarInput[3] & 0xFFFFFFFFu);

    if (RefusesBootloaderWindow(addressHi, addressLo, "AsyncWrite")) {
        return kIOReturnNotPermitted;
    }

    if (length != actualPayloadSize) {
        ASFW_LOG(UserClient, "AsyncWrite: Length mismatch (specified=%u actual=%u)", length,
                 actualPayloadSize);
        return kIOReturnBadArgument;
    }

    ASFW_LOG(UserClient,
             "AsyncWrite: instance=%llu epoch=%llu node=%u addr=0x%04x:%08x len=%u",
             route->deviceInstanceId.value, route->routeEpoch, route->nodeId, addressHi,
             addressLo, length);

    using namespace ASFW::Async;
    auto* asyncPort = GetAsyncSubsystemPort(driver_);
    if (!asyncPort) {
        ASFW_LOG(UserClient, "AsyncWrite: Async port not available");
        return kIOReturnNotReady;
    }

    // Get payload bytes (payloadData already validated above)
    const void* payload = payloadData->getBytesNoCopy();
    if (!payload) {
        ASFW_LOG(UserClient, "AsyncWrite: Failed to get payload bytes");
        return kIOReturnBadArgument;
    }

    // Build WriteParams
    WriteParams params{};
    params.destinationID = route->nodeId;
    params.addressHigh = addressHi;
    params.addressLow = addressLo;
    params.payload = payload;
    params.length = length;

    // Initiate async write with completion callback
    userClient->retain();
    AsyncHandle handle = asyncPort->Write(
        params, [userClient, route = *route](AsyncHandle handle, AsyncStatus status, uint8_t responseCode,
                             std::span<const uint8_t> responsePayload) {
            AsyncCompletionCallback(handle, status, responseCode, userClient, route,
                                    responsePayload.data(),
                                    static_cast<uint32_t>(responsePayload.size()));
            userClient->release();
        });
    if (!handle) {
        userClient->release();
        ASFW_LOG(UserClient, "AsyncWrite: Failed to initiate transaction");
        return kIOReturnError;
    }

    args->scalarOutput[0] = handle.value;
    args->scalarOutputCount = 1;

    ASFW_LOG(UserClient, "AsyncWrite: Initiated with handle=0x%04x (with completion callback)",
             handle.value);
    return kIOReturnSuccess;
}

kern_return_t TransactionHandler::AsyncBlockRead(IOUserClientMethodArguments* args,
                                                 ASFWDriverUserClient* userClient) {
    // Input: DeviceInstanceId[64], addressHi[16], addressLo[32], length[32]
    // Output: handle[16]
    if (!args || args->scalarInputCount < 4 || args->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    const auto route = ResolveDeviceRoute(driver_, args->scalarInput[0], "AsyncBlockRead");
    if (!route.has_value()) {
        return kIOReturnNotReady;
    }
    const uint16_t addressHi = static_cast<uint16_t>(args->scalarInput[1] & 0xFFFF);
    const uint32_t addressLo = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFFFFFFu);
    const uint32_t length = static_cast<uint32_t>(args->scalarInput[3] & 0xFFFFFFFFu);

    ASFW_LOG(UserClient,
             "AsyncBlockRead: instance=%llu epoch=%llu node=%u addr=0x%04x:%08x len=%u",
             route->deviceInstanceId.value, route->routeEpoch, route->nodeId, addressHi,
             addressLo, length);

    using namespace ASFW::Async;
    auto* asyncPort = GetAsyncSubsystemPort(driver_);
    if (!asyncPort) {
        ASFW_LOG(UserClient, "AsyncBlockRead: Async port not available");
        return kIOReturnNotReady;
    }

    ReadParams params{};
    params.destinationID = route->nodeId;
    params.addressHigh = addressHi;
    params.addressLow = addressLo;
    params.length = length;
    params.forceBlock = true;

    userClient->retain();
    AsyncHandle handle = asyncPort->Read(
        params, [userClient, route = *route](AsyncHandle handle, AsyncStatus status, uint8_t responseCode,
                             std::span<const uint8_t> responsePayload) {
            AsyncCompletionCallback(handle, status, responseCode, userClient, route,
                                    responsePayload.data(),
                                    static_cast<uint32_t>(responsePayload.size()));
            userClient->release();
        });
    if (!handle) {
        userClient->release();
        ASFW_LOG(UserClient, "AsyncBlockRead: Failed to initiate transaction");
        return kIOReturnError;
    }

    args->scalarOutput[0] = handle.value;
    args->scalarOutputCount = 1;

    ASFW_LOG(UserClient, "AsyncBlockRead: Initiated with handle=0x%04x (with completion callback)",
             handle.value);
    return kIOReturnSuccess;
}

kern_return_t TransactionHandler::AsyncBlockWrite(IOUserClientMethodArguments* args,
                                                  ASFWDriverUserClient* userClient) {
    // Input: DeviceInstanceId[64], addressHi[16], addressLo[32], length[32]
    // structureInput: payload data
    // Output: handle[16]
    if (!args || args->scalarInputCount < 4 || args->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    if (!args->structureInput) {
        ASFW_LOG(UserClient, "AsyncBlockWrite: No payload data provided");
        return kIOReturnBadArgument;
    }

    OSData* payloadData = OSDynamicCast(OSData, args->structureInput);
    if (!payloadData) {
        ASFW_LOG(UserClient, "AsyncBlockWrite: structureInput is not OSData");
        return kIOReturnBadArgument;
    }

    const uint32_t actualPayloadSize = static_cast<uint32_t>(payloadData->getLength());
    if (actualPayloadSize == 0) {
        ASFW_LOG(UserClient, "AsyncBlockWrite: Empty payload");
        return kIOReturnBadArgument;
    }

    const auto route = ResolveDeviceRoute(driver_, args->scalarInput[0], "AsyncBlockWrite");
    if (!route.has_value()) {
        return kIOReturnNotReady;
    }
    const uint16_t addressHi = static_cast<uint16_t>(args->scalarInput[1] & 0xFFFF);
    const uint32_t addressLo = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFFFFFFu);
    const uint32_t length = static_cast<uint32_t>(args->scalarInput[3] & 0xFFFFFFFFu);

    if (RefusesBootloaderWindow(addressHi, addressLo, "AsyncBlockWrite")) {
        return kIOReturnNotPermitted;
    }

    if (length != actualPayloadSize) {
        ASFW_LOG(UserClient, "AsyncBlockWrite: Length mismatch (specified=%u actual=%u)", length,
                 actualPayloadSize);
        return kIOReturnBadArgument;
    }

    ASFW_LOG(UserClient,
             "AsyncBlockWrite: instance=%llu epoch=%llu node=%u addr=0x%04x:%08x len=%u",
             route->deviceInstanceId.value, route->routeEpoch, route->nodeId, addressHi,
             addressLo, length);

    using namespace ASFW::Async;
    auto* asyncPort = GetAsyncSubsystemPort(driver_);
    if (!asyncPort) {
        ASFW_LOG(UserClient, "AsyncBlockWrite: Async port not available");
        return kIOReturnNotReady;
    }

    const void* payload = payloadData->getBytesNoCopy();
    if (!payload) {
        ASFW_LOG(UserClient, "AsyncBlockWrite: Failed to get payload bytes");
        return kIOReturnBadArgument;
    }

    WriteParams params{};
    params.destinationID = route->nodeId;
    params.addressHigh = addressHi;
    params.addressLow = addressLo;
    params.payload = payload;
    params.length = length;
    params.forceBlock = true;

    userClient->retain();
    AsyncHandle handle = asyncPort->Write(
        params, [userClient, route = *route](AsyncHandle handle, AsyncStatus status, uint8_t responseCode,
                             std::span<const uint8_t> responsePayload) {
            AsyncCompletionCallback(handle, status, responseCode, userClient, route,
                                    responsePayload.data(),
                                    static_cast<uint32_t>(responsePayload.size()));
            userClient->release();
        });
    if (!handle) {
        userClient->release();
        ASFW_LOG(UserClient, "AsyncBlockWrite: Failed to initiate transaction");
        return kIOReturnError;
    }

    args->scalarOutput[0] = handle.value;
    args->scalarOutputCount = 1;

    ASFW_LOG(UserClient, "AsyncBlockWrite: Initiated with handle=0x%04x (with completion callback)",
             handle.value);
    return kIOReturnSuccess;
}

kern_return_t TransactionHandler::GetTransactionResult(IOUserClientMethodArguments* args) {
    // Input: handle[16]
    // Output: status[32], dataLength[32], responseCode[8], data[buffer]
    if (!args || args->scalarInputCount < 1) {
        return kIOReturnBadArgument;
    }

    if (!storage_) {
        return kIOReturnNotReady;
    }

    const uint16_t handle = static_cast<uint16_t>(args->scalarInput[0] & 0xFFFF);

    storage_->Lock();

    // Search for result with matching handle
    size_t index = 0;
    TransactionResult* foundResult = storage_->FindResult(handle, &index);

    if (!foundResult) {
        storage_->Unlock();
        ASFW_LOG(UserClient, "GetTransactionResult: handle=0x%04x not found", handle);
        return kIOReturnNotFound;
    }

    // Copy result to output
    if (args->scalarOutput && args->scalarOutputCount >= 3) {
        args->scalarOutput[0] = foundResult->status;
        args->scalarOutput[1] = foundResult->dataLength;
        args->scalarOutput[2] = foundResult->responseCode;
        args->scalarOutputCount = 3;
    }

    const void* resultBytes = foundResult->Data();
    OSData* resultData = OSData::withBytes(resultBytes, foundResult->dataLength);
    if (resultData) {
        args->structureOutput = resultData;
        args->structureOutputDescriptor = nullptr;
    } else {
        storage_->Unlock();
        return kIOReturnNoMemory;
    }

    ASFW_LOG(UserClient, "GetTransactionResult: handle=0x%04x status=%u rCode=0x%02x len=%u",
             handle, foundResult->status, foundResult->responseCode, foundResult->dataLength);

    // Remove this result from the buffer
    storage_->RemoveResultAtIndex(index);

    storage_->Unlock();
    return kIOReturnSuccess;
}

kern_return_t TransactionHandler::RegisterTransactionListener(IOUserClientMethodArguments* args,
                                                              ASFWDriverUserClient* userClient) {
    // Register async callback for transaction completion notifications
    if (!args || !args->completion) {
        return kIOReturnBadArgument;
    }

    if (!userClient || !userClient->ivars || !userClient->ivars->driver) {
        return kIOReturnNotReady;
    }

    if (!userClient->ivars->actionLock) {
        return kIOReturnNotReady;
    }

    IOLockLock(userClient->ivars->actionLock);
    if (userClient->ivars->transactionAction) {
        userClient->ivars->transactionAction->release();
        userClient->ivars->transactionAction = nullptr;
    }

    args->completion->retain();
    userClient->ivars->transactionAction = args->completion;
    userClient->ivars->transactionListenerRegistered = true;
    userClient->ivars->stopping = false;
    IOLockUnlock(userClient->ivars->actionLock);

    ASFW_LOG(UserClient, "RegisterTransactionListener: callback registered");
    return kIOReturnSuccess;
}

kern_return_t TransactionHandler::AsyncCompareSwap(IOUserClientMethodArguments* args,
                                                   ASFWDriverUserClient* userClient) {
    // Input scalars: DeviceInstanceId[64], addressHi[16], addressLo[32], size[8]
    // structureInput: compareValue (4 or 8 bytes) + newValue (4 or 8 bytes)
    // Output: handle[16], locked[8]

    if (!args || args->scalarInputCount < 4 || args->scalarOutputCount < 2) {
        ASFW_LOG(UserClient, "AsyncCompareSwap: Invalid argument counts");
        return kIOReturnBadArgument;
    }

    if (!args->structureInput) {
        ASFW_LOG(UserClient, "AsyncCompareSwap: No operand data provided");
        return kIOReturnBadArgument;
    }

    // Get operand from structureInput (OSData)
    OSData* operandData = OSDynamicCast(OSData, args->structureInput);
    if (!operandData) {
        ASFW_LOG(UserClient, "AsyncCompareSwap: structureInput is not OSData");
        return kIOReturnBadArgument;
    }

    const auto route = ResolveDeviceRoute(driver_, args->scalarInput[0], "AsyncCompareSwap");
    if (!route.has_value()) {
        return kIOReturnNotReady;
    }
    const uint16_t addressHi = static_cast<uint16_t>(args->scalarInput[1] & 0xFFFF);
    const uint32_t addressLo = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFFFFFFu);
    const uint8_t size = static_cast<uint8_t>(args->scalarInput[3] & 0xFF); // 4 or 8 bytes

    if (RefusesBootloaderWindow(addressHi, addressLo, "AsyncCompareSwap")) {
        return kIOReturnNotPermitted;
    }

    // Validate size (32-bit = 4 bytes, 64-bit = 8 bytes)
    if (size != 4 && size != 8) {
        ASFW_LOG(UserClient, "AsyncCompareSwap: Invalid size=%u (must be 4 or 8)", size);
        return kIOReturnBadArgument;
    }

    // Operand should be compareValue + newValue (size * 2)
    const uint32_t expectedOperandSize = size * 2;
    const uint32_t actualOperandSize = static_cast<uint32_t>(operandData->getLength());
    if (actualOperandSize != expectedOperandSize) {
        ASFW_LOG(UserClient, "AsyncCompareSwap: Operand size mismatch (expected=%u actual=%u)",
                 expectedOperandSize, actualOperandSize);
        return kIOReturnBadArgument;
    }

    ASFW_LOG(UserClient,
             "AsyncCompareSwap: instance=%llu epoch=%llu node=%u addr=0x%04x:%08x size=%u",
             route->deviceInstanceId.value, route->routeEpoch, route->nodeId, addressHi,
             addressLo, size);

    using namespace ASFW::Async;
    auto* asyncPort = GetAsyncSubsystemPort(driver_);
    if (!asyncPort) {
        ASFW_LOG(UserClient, "AsyncCompareSwap: Async port not available");
        return kIOReturnNotReady;
    }

    // Get operand bytes (compareValue + newValue concatenated)
    const void* operand = operandData->getBytesNoCopy();
    if (!operand) {
        ASFW_LOG(UserClient, "AsyncCompareSwap: Failed to get operand bytes");
        return kIOReturnBadArgument;
    }

    // Build LockParams
    LockParams params{};
    params.destinationID = route->nodeId;
    params.addressHigh = addressHi;
    params.addressLow = addressLo;
    params.operand = operand;
    params.operandLength = expectedOperandSize; // compare + swap quadlets
    params.responseLength = size;               // IEEE 1394 returns old value (size bytes)

    // Extended tCode for compare-swap
    // From IEEE 1394-1995: 0x02 = CompareSwap (32/64-bit atomic)
    const uint16_t extendedTCode = 0x02;

    // Initiate async compare-swap with completion callback
    // NOTE: Lock completion includes old value in response payload
    userClient->retain();
    AsyncHandle handle = asyncPort->Lock(
        params, extendedTCode,
        [userClient, route = *route](AsyncHandle handle, AsyncStatus status, uint8_t responseCode,
                     std::span<const uint8_t> responsePayload) {
            // For compare-swap, responsePayload contains the old value read from memory
            // locked = true if compare succeeded (old == compare), false otherwise
            bool locked = (status == AsyncStatus::kSuccess);

            // Store result with lock status and old value
            AsyncCompletionCallback(handle, status, responseCode, userClient, route,
                                    responsePayload.data(),
                                    static_cast<uint32_t>(responsePayload.size()));

            ASFW_LOG(UserClient, "AsyncCompareSwap completion: handle=0x%04x locked=%{public}s",
                     handle.value, locked ? "YES" : "NO");
            userClient->release();
        });

    if (!handle) {
        userClient->release();
        ASFW_LOG(UserClient, "AsyncCompareSwap: Failed to initiate transaction");
        return kIOReturnError;
    }

    // Return handle and preliminary lock status (actual result comes via callback)
    args->scalarOutput[0] = handle.value;
    args->scalarOutput[1] = 0; // locked status unknown until completion
    args->scalarOutputCount = 2;

    ASFW_LOG(UserClient,
             "AsyncCompareSwap: Initiated with handle=0x%04x (with completion callback)",
             handle.value);
    return kIOReturnSuccess;
}

} // namespace ASFW::UserClient
