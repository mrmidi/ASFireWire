#pragma once

#include <cstring>
#include <cstdint>
#include <vector>

#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>

#include "../../Logging/Logging.hpp"
#include "../../Protocols/SBP2/AddressSpaceManager.hpp"
#include "../../Protocols/SBP2/SCSICommandSet.hpp"
#include "../../Protocols/SBP2/SBP2SessionRegistry.hpp"
#include "../WireFormats/SBP2CommandWireFormats.hpp"

namespace ASFW::UserClient {

class SBP2Handler {
public:
    explicit SBP2Handler(ASFW::Protocols::SBP2::AddressSpaceManager* manager,
                         ASFW::Protocols::SBP2::SBP2SessionRegistry* registry)
        : manager_(manager), registry_(registry) {}

    ~SBP2Handler() = default;

    SBP2Handler(const SBP2Handler&) = delete;
    SBP2Handler& operator=(const SBP2Handler&) = delete;

    // Address space management (selectors 46-49)
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
            owner,
            addressHi,
            addressLo,
            length,
            &handle,
            nullptr);
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
            ASFW_LOG(UserClient,
                     "ReadIncomingData: owner=%p handle=0x%llx offset=%u len=%u -> kr=0x%x",
                     owner,
                     static_cast<unsigned long long>(handle),
                     offset,
                     length,
                     static_cast<unsigned int>(kr));
            return kr;
        }

        OSData* output = OSData::withBytes(data.data(), static_cast<uint32_t>(data.size()));
        if (!output) {
            ASFW_LOG(UserClient,
                     "ReadIncomingData: owner=%p handle=0x%llx offset=%u len=%u -> no memory",
                     owner,
                     static_cast<unsigned long long>(handle),
                     offset,
                     length);
            return kIOReturnNoMemory;
        }

        ASFW_LOG(UserClient,
                 "ReadIncomingData: owner=%p handle=0x%llx offset=%u len=%u -> %u bytes",
                 owner,
                 static_cast<unsigned long long>(handle),
                 offset,
                 length,
                 static_cast<unsigned int>(data.size()));
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
            owner,
            handle,
            offset,
            std::span<const uint8_t>(bytes, length));
    }

    void ReleaseOwner(void* owner) {
        if (manager_) {
            manager_->ReleaseOwner(owner);
        }
        if (registry_) {
            registry_->ReleaseOwner(owner);
        }
    }

    // Session management (selectors 53-58)

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

    kern_return_t StartSBP2Login(IOUserClientMethodArguments* args) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        return registry_->StartLogin(handle) ? kIOReturnSuccess : kIOReturnError;
    }

    kern_return_t GetSBP2SessionState(IOUserClientMethodArguments* args) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        auto state = registry_->GetSessionState(handle);
        if (!state.has_value()) {
            return kIOReturnNotFound;
        }

        // Return as scalars: loginState, loginID, generation, lastError, reconnectPending
        if (args->scalarOutput && args->scalarOutputCount >= 5) {
            args->scalarOutput[0] = static_cast<uint64_t>(state->loginState);
            args->scalarOutput[1] = static_cast<uint64_t>(state->loginID);
            args->scalarOutput[2] = static_cast<uint64_t>(state->generation);
            args->scalarOutput[3] = static_cast<uint64_t>(static_cast<uint32_t>(state->lastError));
            args->scalarOutput[4] = state->reconnectPending ? 1 : 0;
            args->scalarOutputCount = 5;
        }
        return kIOReturnSuccess;
    }

    kern_return_t SubmitSBP2Inquiry(IOUserClientMethodArguments* args) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 2) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        const uint8_t allocationLength = static_cast<uint8_t>(args->scalarInput[1] & 0xFFu);
        return registry_->SubmitInquiry(handle, allocationLength) ? kIOReturnSuccess : kIOReturnError;
    }

    kern_return_t GetSBP2InquiryResult(IOUserClientMethodArguments* args) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        auto result = registry_->GetInquiryResult(handle);
        if (!result.has_value()) {
            return kIOReturnNotFound;
        }

        OSData* output = OSData::withBytes(result->data(), static_cast<uint32_t>(result->size()));
        if (!output) {
            return kIOReturnNoMemory;
        }

        args->structureOutput = output;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    kern_return_t SubmitSBP2Command(IOUserClientMethodArguments* args) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1 || !args->structureInput) {
            return kIOReturnBadArgument;
        }

        OSData* input = OSDynamicCast(OSData, args->structureInput);
        if (!input) {
            return kIOReturnBadArgument;
        }

        const auto* bytes = static_cast<const uint8_t*>(input->getBytesNoCopy());
        const size_t inputLength = input->getLength();
        if (!bytes || inputLength < sizeof(Wire::SBP2CommandRequestWire)) {
            return kIOReturnBadArgument;
        }

        const auto* header = reinterpret_cast<const Wire::SBP2CommandRequestWire*>(bytes);
        const size_t expectedLength =
            sizeof(Wire::SBP2CommandRequestWire) +
            static_cast<size_t>(header->cdbLength) +
            static_cast<size_t>(header->outgoingLength);
        if (inputLength != expectedLength || header->cdbLength == 0) {
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
        return registry_->SubmitCommand(handle, request) ? kIOReturnSuccess : kIOReturnError;
    }

    kern_return_t GetSBP2CommandResult(IOUserClientMethodArguments* args) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        auto result = registry_->GetCommandResult(handle);
        if (!result.has_value()) {
            return kIOReturnNotFound;
        }

        Wire::SBP2CommandResultWire header{};
        header.transportStatus = result->transportStatus;
        header.sbpStatus = result->sbpStatus;
        header.payloadLength = static_cast<uint32_t>(result->payload.size());
        header.senseLength = static_cast<uint32_t>(result->senseData.size());

        std::vector<uint8_t> serialized(
            sizeof(Wire::SBP2CommandResultWire) +
            result->payload.size() +
            result->senseData.size());
        memcpy(serialized.data(), &header, sizeof(header));

        size_t offset = sizeof(Wire::SBP2CommandResultWire);
        if (!result->payload.empty()) {
            memcpy(serialized.data() + offset, result->payload.data(), result->payload.size());
            offset += result->payload.size();
        }
        if (!result->senseData.empty()) {
            memcpy(serialized.data() + offset, result->senseData.data(), result->senseData.size());
        }

        OSData* output = OSData::withBytes(serialized.data(), static_cast<uint32_t>(serialized.size()));
        if (!output) {
            return kIOReturnNoMemory;
        }

        args->structureOutput = output;
        args->structureOutputDescriptor = nullptr;
        return kIOReturnSuccess;
    }

    kern_return_t ReleaseSBP2Session(IOUserClientMethodArguments* args, void* owner) {
        if (!registry_) {
            return kIOReturnNotReady;
        }
        if (!args || !args->scalarInput || args->scalarInputCount < 1) {
            return kIOReturnBadArgument;
        }

        const uint64_t handle = args->scalarInput[0];
        return registry_->ReleaseSession(handle) ? kIOReturnSuccess : kIOReturnNotFound;
    }

private:
    ASFW::Protocols::SBP2::AddressSpaceManager* manager_{nullptr};
    ASFW::Protocols::SBP2::SBP2SessionRegistry* registry_{nullptr};
};

} // namespace ASFW::UserClient
