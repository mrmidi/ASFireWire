//
// ASFWSBP2Nub.cpp
// ASFWDriver
//
// Minimal provider nub for ASFWSCSIController. See ASFWSBP2Nub.iig.
//

#include <net.mrmidi.ASFW.ASFWDriver/ASFWSBP2Nub.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSSharedPtr.h>

#include "../Logging/Logging.hpp"

kern_return_t IMPL(ASFWSBP2Nub, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // Adopt the app-provisioned DART mapper id staged on the parent driver as
    // iommu-parent BEFORE registering: the SCSI kernel shim resolves its DMA
    // mapper from this nub the moment the HBA personality matches, so the
    // property must be in place first (see PublishSBP2Nub in ASFWDriver.cpp).
    // Absent on Intel and when the plist dict already carries iommu-parent.
    OSContainer* rawStaged = nullptr;
    if (SearchProperty("ASFWSBP2MapperID", "IOService", kIOServiceSearchPropertyParents,
                       &rawStaged) == kIOReturnSuccess) {
        auto staged = OSSharedPtr(rawStaged, OSNoRetain);
        if (auto* data = OSDynamicCast(OSData, staged.get()); data != nullptr) {
            OSDictionary* rawProps = nullptr;
            if (CopyProperties(&rawProps) == kIOReturnSuccess && rawProps != nullptr) {
                auto props = OSSharedPtr(rawProps, OSNoRetain);
                props->setObject("iommu-parent", data);
                const kern_return_t pkr = SetProperties(props.get());
                ASFW_LOG(Controller, "[SCSIHBA] nub adopted staged mapper id: 0x%08x", pkr);
            }
        }
    }

    ASFW_LOG(Controller, "[SCSIHBA] ASFWSBP2Nub::Start — registering (phase-0)");
    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] ASFWSBP2Nub RegisterService failed: 0x%x", ret);
    }
    return ret;
}

kern_return_t IMPL(ASFWSBP2Nub, Stop)
{
    ASFW_LOG(Controller, "[SCSIHBA] ASFWSBP2Nub::Stop");
    return Stop(provider, SUPERDISPATCH);
}
