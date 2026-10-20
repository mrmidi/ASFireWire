//
// ASFWSBP2Nub.cpp
// ASFWDriver
//
// Per-discovered-unit provider nub. See ASFWSBP2Nub.iig.
//

#include <net.asfw.driver/ASFWSBP2Nub.h>

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

#include "../Logging/Logging.hpp"

kern_return_t IMPL(ASFWSBP2Nub, Start)
{
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    ASFW_LOG(Controller, "[SCSIHBA] ASFWSBP2Nub::Start — registering discovered unit");
    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[SCSIHBA] ASFWSBP2Nub RegisterService failed: 0x%x", ret);
    }
    return ret;
}

kern_return_t IMPL(ASFWSBP2Nub, Stop)
{
    ASFW_LOG(Controller, "[Lifecycle] ASFWSBP2Nub Stop enter obj=%p provider=%p", this, provider);
    const kern_return_t kr = Stop(provider, SUPERDISPATCH);
    ASFW_LOG(Controller, "[Lifecycle] ASFWSBP2Nub Stop done obj=%p kr=0x%x", this, kr);
    return kr;
}

void ASFWSBP2Nub::free()
{
    ASFW_LOG(Controller, "[Lifecycle] ASFWSBP2Nub free obj=%p", this);
    super::free();
}
