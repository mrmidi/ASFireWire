#pragma once

// SBP2Handler — user-client boundary for the SBP-2 address-space + session/command
// layer. Foundation methods (address-range alloc/dealloc/read/write) operate on the
// AddressSpaceManager; session methods (FW-56's SessionRegistry) expose create/login/
// state/inquiry/command/task-management/release across the DriverKit selector ABI.
//
// Ported from PR #19 (re-threaded onto DICE's decomposed SessionRegistry). The
// owner-validation contract is load-bearing: every session call passes the opaque
// (void* owner, uint64_t handle) pair straight through to the registry, which
// rejects cross-owner access (8b64806). `registry` defaults to null so the existing
// address-space-only construction keeps working until the registry is wired into the
// driver lifecycle (FW-58).

#include <cstring>
#include <cstdint>
#include <span>
#include <vector>

#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>

#include "../../Logging/Logging.hpp"
#include "../../Protocols/SBP2/AddressSpaceManager.hpp"
#include "../../Protocols/SBP2/SCSICommandSet.hpp"
#include "../../Protocols/SBP2/Session/SessionRegistry.hpp"
#include "../WireFormats/SBP2CommandWireFormats.hpp"

namespace ASFW::UserClient {

class SBP2Handler {
public:
    explicit SBP2Handler(ASFW::Protocols::SBP2::AddressSpaceManager* manager,
                         ASFW::Protocols::SBP2::SessionRegistry* registry = nullptr)
        : manager_(manager), registry_(registry) {}

    ~SBP2Handler() = default;

    SBP2Handler(const SBP2Handler&) = delete;
    SBP2Handler& operator=(const SBP2Handler&) = delete;

    // -----------------------------------------------------------------------
    // Address space management
    // -----------------------------------------------------------------------

    kern_return_t AllocateAddressRange(IOUserClientMethodArguments* args, void* owner) {
        if (!manager_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 3 ||
            !args->scalarOutput || args->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint16_t addressHi = static_cast<uint16_t>(args->scalarInput[0] & 0xFFFFu);
        const uint32_t addressLo = static_cast<uint32_t>(args->scalarInput[1] & 0xFFFF'FFFFu);
        const uint32_t length = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFF'FFFFu);

        uint64_t handle = 0;
        const kern_return_t kr = manager_->AllocateAddressRange(
            owner, addressHi, addressLo, length, &handle, nullptr);
        if (kr != kIOReturnSuccess) {
            return kr;
        }

        args->scalarOutput[0] = handle;
        args->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    kern_return_t DeallocateAddressRange(IOUserClientMethodArguments* args, void* owner) {
        if (!manager_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        return manager_->DeallocateAddressRange(owner, handle);
    }

    kern_return_t ReadIncomingData(IOUserClientMethodArguments* args, void* owner) {
        if (!manager_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 3) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        const uint32_t offset = static_cast<uint32_t>(args->scalarInput[1] & 0xFFFF'FFFFu);
        const uint32_t length = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFF'FFFFu);

        std::vector<uint8_t> data;
        const kern_return_t kr = manager_->ReadIncomingData(owner, handle, offset, length, &data);
        if (kr != kIOReturnSuccess) {
            return kr;
        }

        OSData* output = OSData::withBytes(data.data(), static_cast<uint32_t>(data.size()));
        if (!output) {
            return kIOReturnNoMemory;
        }

        args->structureOutput = output;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    kern_return_t WriteLocalData(IOUserClientMethodArguments* args, void* owner) {
        if (!manager_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 3 || !args->structureInput) {
            return kIOReturnBadArgument;
        }

        OSData* payloadData = OSDynamicCast(OSData, args->structureInput);
        if (!payloadData) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        const uint32_t offset = static_cast<uint32_t>(args->scalarInput[1] & 0xFFFF'FFFFu);
        const uint32_t length = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFF'FFFFu);

        if (payloadData->getLength() != length) {
            return kIOReturnBadArgument;
        }

        const auto* bytes = static_cast<const uint8_t*>(payloadData->getBytesNoCopy());
        if (!bytes && length > 0) {
            return kIOReturnBadArgument;
        }

        return manager_->WriteLocalData(
            owner, handle, offset, std::span<const uint8_t>(bytes, length));
    }

    // Release every address range and session owned by this client.
    void ReleaseOwner(void* owner) {
        if (registry_) {
            registry_->ReleaseOwner(owner);  // sessions before address ranges (9ca0d8e)
        }
        if (manager_) {
            manager_->ReleaseOwner(owner);
        }
    }

    // -----------------------------------------------------------------------
    // Session / command management
    // -----------------------------------------------------------------------

    kern_return_t CreateSBP2Session(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 3 ||
            !args->scalarOutput || args->scalarOutputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint32_t guidHi = static_cast<uint32_t>(args->scalarInput[0] & 0xFFFF'FFFFu);
        const uint32_t guidLo = static_cast<uint32_t>(args->scalarInput[1] & 0xFFFF'FFFFu);
        const uint32_t romOffset = static_cast<uint32_t>(args->scalarInput[2] & 0xFFFF'FFFFu);
        const uint64_t guid = (static_cast<uint64_t>(guidHi) << 32) | guidLo;

        auto result = registry_->CreateSession(owner, guid, romOffset);
        if (!result.has_value()) {
            return result.error();
        }

        args->scalarOutput[0] = *result;
        args->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }

    kern_return_t StartSBP2Login(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        return registry_->StartLogin(owner, handle) ? kIOReturnSuccess : kIOReturnError;
    }

    kern_return_t GetSBP2SessionState(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1 ||
            !args->scalarOutput || args->scalarOutputCount < 5) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        auto state = registry_->GetSessionState(owner, handle);
        if (!state.has_value()) {
            return kIOReturnNotFound;
        }

        // Return as scalars: loginState, loginID, generation, lastError, reconnectPending.
        args->scalarOutput[0] = static_cast<uint64_t>(state->loginState);
        args->scalarOutput[1] = static_cast<uint64_t>(state->loginID);
        args->scalarOutput[2] = static_cast<uint64_t>(state->generation);
        args->scalarOutput[3] = static_cast<uint64_t>(static_cast<uint32_t>(state->lastError));
        args->scalarOutput[4] = state->reconnectPending ? 1 : 0;
        args->scalarOutputCount = 5;
        return kIOReturnSuccess;
    }

    kern_return_t SubmitSBP2Inquiry(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 2) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        const uint8_t allocationLength = static_cast<uint8_t>(args->scalarInput[1] & 0xFFu);
        return registry_->SubmitInquiry(owner, handle, allocationLength) ? kIOReturnSuccess
                                                                         : kIOReturnError;
    }

    kern_return_t GetSBP2InquiryResult(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        auto result = registry_->GetInquiryResult(owner, handle);
        if (!result.has_value()) {
            return kIOReturnNotFound;
        }
        if (result->transportStatus != 0) {
            return result->transportStatus > 0
                       ? static_cast<kern_return_t>(result->transportStatus)
                       : kIOReturnError;
        }
        if (result->sbpStatus != ASFW::Protocols::SBP2::Wire::SBPStatus::kNoAdditionalInfo) {
            return kIOReturnError;
        }

        OSData* output = OSData::withBytes(result->payload.data(),
                                           static_cast<uint32_t>(result->payload.size()));
        if (!output) {
            return kIOReturnNoMemory;
        }

        args->structureOutput = output;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    kern_return_t SubmitSBP2Command(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1 ||
            (!args->structureInput && !args->structureInputDescriptor)) {
            return kIOReturnBadArgument;
        }

        // Inputs past the inband limit (~4 KB — e.g. SEND LUT's 32 KB data-OUT
        // payload) arrive as a memory descriptor instead of inline OSData.
        const uint8_t* bytes = nullptr;
        size_t inputLength = 0;
        std::vector<uint8_t> mappedInput;
        if (args->structureInput) {
            OSData* input = OSDynamicCast(OSData, args->structureInput);
            if (!input) {
                return kIOReturnBadArgument;
            }
            bytes = static_cast<const uint8_t*>(input->getBytesNoCopy());
            inputLength = input->getLength();
        } else {
            constexpr uint64_t kMaxInputLength =
                sizeof(Wire::SBP2CommandRequestWire) +
                Wire::kSBP2CommandMaxCDBLength +
                Wire::kSBP2CommandMaxTransferLength;
            uint64_t descLength = 0;
            if (args->structureInputDescriptor->GetLength(&descLength) != kIOReturnSuccess ||
                descLength < sizeof(Wire::SBP2CommandRequestWire) ||
                descLength > kMaxInputLength) {
                return kIOReturnBadArgument;
            }
            IOMemoryMap* map = nullptr;
            const kern_return_t mapKr =
                args->structureInputDescriptor->CreateMapping(0, 0, 0, 0, 0, &map);
            if (mapKr != kIOReturnSuccess || !map) {
                return kIOReturnVMError;
            }
            const auto* src = reinterpret_cast<const uint8_t*>(map->GetAddress());
            if (src) {
                mappedInput.assign(src, src + descLength);
            }
            map->release();
            bytes = mappedInput.data();
            inputLength = mappedInput.size();
        }
        if (!bytes || inputLength < sizeof(Wire::SBP2CommandRequestWire)) {
            return kIOReturnBadArgument;
        }

        const auto* header = reinterpret_cast<const Wire::SBP2CommandRequestWire*>(bytes);
        if (header->cdbLength == 0 || header->cdbLength > Wire::kSBP2CommandMaxCDBLength ||
            header->transferLength > Wire::kSBP2CommandMaxTransferLength ||
            header->captureSenseData > 1 || header->_reserved[0] != 0 ||
            header->_reserved[1] != 0) {
            return kIOReturnBadArgument;
        }

        const size_t expectedLength = sizeof(Wire::SBP2CommandRequestWire) +
                                      static_cast<size_t>(header->cdbLength) +
                                      static_cast<size_t>(header->outgoingLength);
        if (inputLength != expectedLength) {
            return kIOReturnBadArgument;
        }

        Protocols::SBP2::SCSI::DataDirection direction{};
        switch (header->direction) {
        case 0:
            direction = Protocols::SBP2::SCSI::DataDirection::None;
            break;
        case 1:
            direction = Protocols::SBP2::SCSI::DataDirection::FromTarget;
            break;
        case 2:
            direction = Protocols::SBP2::SCSI::DataDirection::ToTarget;
            break;
        default:
            return kIOReturnBadArgument;
        }

        if (direction == Protocols::SBP2::SCSI::DataDirection::ToTarget) {
            if (header->outgoingLength != header->transferLength) {
                return kIOReturnBadArgument;
            }
        } else if (header->outgoingLength != 0) {
            return kIOReturnBadArgument;
        }
        if (direction == Protocols::SBP2::SCSI::DataDirection::None && header->transferLength != 0) {
            return kIOReturnBadArgument;
        }

        Protocols::SBP2::SCSI::CommandRequest request{};
        request.direction = direction;
        request.transferLength = header->transferLength;
        request.timeoutMs = header->timeoutMs;
        request.captureSenseData = header->captureSenseData != 0;

        const uint8_t* cursor = bytes + sizeof(Wire::SBP2CommandRequestWire);
        request.cdb.assign(cursor, cursor + header->cdbLength);
        cursor += header->cdbLength;
        request.outgoingPayload.assign(cursor, cursor + header->outgoingLength);

        const uint64_t handle = args->scalarInput[0];
        return registry_->SubmitCommand(owner, handle, request) ? kIOReturnSuccess
                                                                : kIOReturnError;
    }

    kern_return_t GetSBP2CommandResult(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        auto result = registry_->GetCommandResult(owner, handle);
        if (!result.has_value()) {
            return kIOReturnNotFound;
        }

        Wire::SBP2CommandResultWire header{};
        header.transportStatus = result->transportStatus;
        header.sbpStatus = result->sbpStatus;
        header.scsiStatusValid = result->scsiStatusValid ? 1 : 0;
        header.scsiStatus = result->scsiStatus;
        header.payloadLength = static_cast<uint32_t>(result->payload.size());
        header.senseLength = static_cast<uint32_t>(result->senseData.size());

        const size_t totalSize = sizeof(Wire::SBP2CommandResultWire) +
                                 result->payload.size() + result->senseData.size();

        // Results past the inband limit (large READ(10) payloads) return through
        // a caller-supplied descriptor — mirror of SubmitSBP2Command's
        // structureInputDescriptor path. The kernel sets it when the caller
        // requests > one inband page of output; setting structureOutput instead
        // is an error per the IOUserClient contract.
        // NOTE: the result was already consumed from the registry above, so a
        // too-small buffer loses it — callers must size from transferLength.
        if (args->structureOutputDescriptor) {
            uint64_t descLength = 0;
            if (args->structureOutputDescriptor->GetLength(&descLength) != kIOReturnSuccess ||
                descLength < totalSize) {
                return kIOReturnNoSpace;
            }
            IOMemoryMap* map = nullptr;
            const kern_return_t mapKr =
                args->structureOutputDescriptor->CreateMapping(0, 0, 0, 0, 0, &map);
            if (mapKr != kIOReturnSuccess || !map) {
                return kIOReturnVMError;
            }
            auto* dst = reinterpret_cast<uint8_t*>(map->GetAddress());
            if (!dst) {
                map->release();
                return kIOReturnVMError;
            }
            std::memcpy(dst, &header, sizeof(header));
            size_t descOffset = sizeof(Wire::SBP2CommandResultWire);
            if (!result->payload.empty()) {
                std::memcpy(dst + descOffset, result->payload.data(), result->payload.size());
                descOffset += result->payload.size();
            }
            if (!result->senseData.empty()) {
                std::memcpy(dst + descOffset, result->senseData.data(),
                            result->senseData.size());
            }
            map->release();
            return kIOReturnSuccess;
        }

        std::vector<uint8_t> serialized(totalSize);
        std::memcpy(serialized.data(), &header, sizeof(header));

        size_t offset = sizeof(Wire::SBP2CommandResultWire);
        if (!result->payload.empty()) {
            std::memcpy(serialized.data() + offset, result->payload.data(), result->payload.size());
            offset += result->payload.size();
        }
        if (!result->senseData.empty()) {
            std::memcpy(serialized.data() + offset, result->senseData.data(),
                        result->senseData.size());
        }

        OSData* output = OSData::withBytes(serialized.data(),
                                           static_cast<uint32_t>(serialized.size()));
        if (!output) {
            return kIOReturnNoMemory;
        }

        args->structureOutput = output;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    kern_return_t SubmitSBP2TaskManagement(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 2) {
            return kIOReturnBadArgument;
        }

        Protocols::SBP2::SBP2ManagementORB::Function function{};
        switch (args->scalarInput[1]) {
        case 0x0C:
            function = Protocols::SBP2::SBP2ManagementORB::Function::AbortTaskSet;
            break;
        case 0x0E:
            function = Protocols::SBP2::SBP2ManagementORB::Function::LogicalUnitReset;
            break;
        case 0x0F:
            function = Protocols::SBP2::SBP2ManagementORB::Function::TargetReset;
            break;
        default:
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        return registry_->SubmitTaskManagement(owner, handle, function) ? kIOReturnSuccess
                                                                        : kIOReturnError;
    }

    kern_return_t ReleaseSBP2Session(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        return registry_->ReleaseSession(owner, handle) ? kIOReturnSuccess : kIOReturnNotFound;
    }

private:
    ASFW::Protocols::SBP2::AddressSpaceManager* manager_{nullptr};
    ASFW::Protocols::SBP2::SessionRegistry* registry_{nullptr};
};

} // namespace ASFW::UserClient
