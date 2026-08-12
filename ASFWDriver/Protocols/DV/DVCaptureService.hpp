#pragma once

#include <cstdint>

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>

#include "../../Common/DriverKitOwnership.hpp"
#include "../../Discovery/DeviceRouteToken.hpp"
#include "DVConnectionPlan.hpp"
#include "DVCaptureSink.hpp"

namespace ASFW::Driver {
class HardwareInterface;
class IsochService;
}

namespace ASFW::Discovery {
class DeviceRegistry;
}

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::CMP {
class CMPClient;
}

namespace ASFW::Protocols::DV {

/// Owns the DV content sink and its shared-memory session. The OHCI transport
/// remains payload-opaque and is borrowed only while this service is active.
class DVCaptureService final {
public:
    DVCaptureService();
    ~DVCaptureService();

    DVCaptureService(const DVCaptureService&) = delete;
    DVCaptureService& operator=(const DVCaptureService&) = delete;

    [[nodiscard]] kern_return_t Start(Discovery::DeviceInstanceId deviceInstanceId,
                                      uint64_t ownerToken,
                                      Driver::IsochService& isoch,
                                      Driver::HardwareInterface& hardware,
                                      Discovery::DeviceRegistry& registry,
                                      IRM::IRMClient& irm,
                                      CMP::CMPClient& cmp);
    [[nodiscard]] kern_return_t Stop(uint64_t ownerToken,
                                     Driver::IsochService& isoch);
    void StopAll(Driver::IsochService& isoch) noexcept;
    void HandleBusReset(Driver::IsochService& isoch) noexcept;

    [[nodiscard]] kern_return_t CopyMemory(
        uint64_t ownerToken,
        uint64_t* options,
        IOMemoryDescriptor** memory) const;

    [[nodiscard]] bool IsActive() const noexcept;

private:
    struct RingMapping final {
        OSSharedPtr<IOBufferMemoryDescriptor> memory{};
        OSSharedPtr<IOMemoryMap> map{};
        uint64_t bytes{0};

        void Reset() noexcept {
            map.reset();
            memory.reset();
            bytes = 0;
        }

        [[nodiscard]] void* BaseAddress() const noexcept {
            return map ? reinterpret_cast<void*>(
                             static_cast<uintptr_t>(map->GetAddress()))
                       : nullptr;
        }
    };

    void ResetState() noexcept;
    [[nodiscard]] kern_return_t TeardownConnection(bool generationInvalid) noexcept;

    mutable IOLock* lock_{nullptr};
    RingMapping ring_{};
    DVCaptureSink sink_{};
    uint64_t ownerToken_{0};
    // Runtime identity and liveness travel together in one route token.
    Discovery::DeviceRouteToken route_{};
    uint8_t channel_{0xFF};
    uint8_t outputPlug_{0xFF};
    uint32_t bandwidthUnits_{0};
    ConnectionMode connectionMode_{ConnectionMode::Initializing};
    IRM::IRMClient* irm_{nullptr};
    CMP::CMPClient* cmp_{nullptr};
    bool pcrConnected_{false};
    bool resourcesAllocated_{false};
    bool active_{false};
};

} // namespace ASFW::Protocols::DV
