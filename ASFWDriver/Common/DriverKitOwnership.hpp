#pragma once

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include <type_traits>

namespace ASFW::Common {

template <typename T>
[[nodiscard]] OSSharedPtr<T> AdoptRetained(T*& rawObject) noexcept
{
    static_assert(std::is_base_of_v<OSObject, T>,
                  "AdoptRetained<T>() requires T to derive from OSObject");
    if (!rawObject) {
        return {};
    }

    OSSharedPtr<T> adopted(rawObject, OSNoRetain);
    rawObject = nullptr;
    return adopted;
}

template <typename T>
[[nodiscard]] kern_return_t CreateSharedMapping(const OSSharedPtr<T>& memory,
                                                OSSharedPtr<IOMemoryMap>& outMap,
                                                uint64_t options = kIOMemoryMapCacheModeDefault) noexcept
{
    outMap.reset();
    if (!memory) {
        return kIOReturnBadArgument;
    }

    IOMemoryMap* rawMap = nullptr;
    const kern_return_t kr = memory->CreateMapping(options, 0, 0, 0, 0, &rawMap);
    if (kr != kIOReturnSuccess || !rawMap) {
        if (rawMap) {
            rawMap->release();
            rawMap = nullptr;
        }
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }

    outMap = AdoptRetained(rawMap);
    return kIOReturnSuccess;
}

} // namespace ASFW::Common
