#pragma once

#include <cstdint>
#include <vector>

#include <DriverKit/IOUserClient.h>
#include <DriverKit/OSData.h>

#include "../../Logging/Logging.hpp"
#include "../../Protocols/SBP2/AddressSpaceManager.hpp"

namespace ASFW::UserClient {

class SBP2Handler {
public:
    explicit SBP2Handler(ASFW::Protocols::SBP2::AddressSpaceManager* manager)
        : manager_(manager) {}

    ~SBP2Handler() = default;

    SBP2Handler(const SBP2Handler&) = delete;
    SBP2Handler& operator=(const SBP2Handler&) = delete;

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
            owner,
            handle,
            offset,
            std::span<const uint8_t>(bytes, length));
    }

    void ReleaseOwner(void* owner) {
        if (manager_) {
            manager_->ReleaseOwner(owner);
        }
    }

private:
    ASFW::Protocols::SBP2::AddressSpaceManager* manager_{nullptr};
};

} // namespace ASFW::UserClient
