//
// ASFWMidiNub.cpp
// ASFWDriver
//

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWMidiNub.h>

#include "../../Logging/Logging.hpp"

kern_return_t IMPL(ASFWMidiNub, Start) {
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: super::Start failed 0x%x", ret);
        return ret;
    }

    // Publish so ASFWMIDIDriver can match. Properties were set by the
    // publisher before this point, so a matching driver sees a complete nub.
    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Midi, "ASFWMidiNub: RegisterService failed 0x%x", ret);
        return ret;
    }

    ASFW_LOG(Midi, "ASFWMidiNub: started");
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWMidiNub, Stop) {
    ASFW_LOG(Midi, "ASFWMidiNub: stopping");
    return Stop(provider, SUPERDISPATCH);
}
