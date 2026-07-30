//
// ASFWSBP2Nub.cpp
// ASFWDriver
//
// Minimal provider nub for ASFWSCSIController. See ASFWSBP2Nub.iig.
//

#include <net.mrmidi.ASFW.ASFWDriver/ASFWSBP2Nub.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

#include "../Logging/Logging.hpp"

kern_return_t IMPL(ASFWSBP2Nub, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
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
