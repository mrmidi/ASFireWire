// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <DriverKit/IOReturn.h>

namespace ASFW::Logging {
// Names come from the build SDK's DriverKit/IOReturn.h. No allocation or I/O;
// unknown families/vendor values retain their numeric code at the call site.
[[nodiscard]] constexpr const char* IOReturnName(IOReturn status) noexcept {
    switch (status) {
        case kIOReturnSuccess: return "kIOReturnSuccess";
        case kIOReturnError: return "kIOReturnError";
        case kIOReturnNoMemory: return "kIOReturnNoMemory";
        case kIOReturnNoResources: return "kIOReturnNoResources";
        case kIOReturnIPCError: return "kIOReturnIPCError";
        case kIOReturnNoDevice: return "kIOReturnNoDevice";
        case kIOReturnNotPrivileged: return "kIOReturnNotPrivileged";
        case kIOReturnBadArgument: return "kIOReturnBadArgument";
        case kIOReturnLockedRead: return "kIOReturnLockedRead";
        case kIOReturnLockedWrite: return "kIOReturnLockedWrite";
        case kIOReturnExclusiveAccess: return "kIOReturnExclusiveAccess";
        case kIOReturnBadMessageID: return "kIOReturnBadMessageID";
        case kIOReturnUnsupported: return "kIOReturnUnsupported";
        case kIOReturnVMError: return "kIOReturnVMError";
        case kIOReturnInternalError: return "kIOReturnInternalError";
        case kIOReturnIOError: return "kIOReturnIOError";
        case kIOReturnCannotLock: return "kIOReturnCannotLock";
        case kIOReturnNotOpen: return "kIOReturnNotOpen";
        case kIOReturnNotReadable: return "kIOReturnNotReadable";
        case kIOReturnNotWritable: return "kIOReturnNotWritable";
        case kIOReturnNotAligned: return "kIOReturnNotAligned";
        case kIOReturnBadMedia: return "kIOReturnBadMedia";
        case kIOReturnStillOpen: return "kIOReturnStillOpen";
        case kIOReturnRLDError: return "kIOReturnRLDError";
        case kIOReturnDMAError: return "kIOReturnDMAError";
        case kIOReturnBusy: return "kIOReturnBusy";
        case kIOReturnTimeout: return "kIOReturnTimeout";
        case kIOReturnOffline: return "kIOReturnOffline";
        case kIOReturnNotReady: return "kIOReturnNotReady";
        case kIOReturnNotAttached: return "kIOReturnNotAttached";
        case kIOReturnNoChannels: return "kIOReturnNoChannels";
        case kIOReturnNoSpace: return "kIOReturnNoSpace";
        case kIOReturnPortExists: return "kIOReturnPortExists";
        case kIOReturnCannotWire: return "kIOReturnCannotWire";
        case kIOReturnNoInterrupt: return "kIOReturnNoInterrupt";
        case kIOReturnNoFrames: return "kIOReturnNoFrames";
        case kIOReturnMessageTooLarge: return "kIOReturnMessageTooLarge";
        case kIOReturnNotPermitted: return "kIOReturnNotPermitted";
        case kIOReturnNoPower: return "kIOReturnNoPower";
        case kIOReturnNoMedia: return "kIOReturnNoMedia";
        case kIOReturnUnformattedMedia: return "kIOReturnUnformattedMedia";
        case kIOReturnUnsupportedMode: return "kIOReturnUnsupportedMode";
        case kIOReturnUnderrun: return "kIOReturnUnderrun";
        case kIOReturnOverrun: return "kIOReturnOverrun";
        case kIOReturnDeviceError: return "kIOReturnDeviceError";
        case kIOReturnNoCompletion: return "kIOReturnNoCompletion";
        case kIOReturnAborted: return "kIOReturnAborted";
        case kIOReturnNoBandwidth: return "kIOReturnNoBandwidth";
        case kIOReturnNotResponding: return "kIOReturnNotResponding";
        case kIOReturnIsoTooOld: return "kIOReturnIsoTooOld";
        case kIOReturnIsoTooNew: return "kIOReturnIsoTooNew";
        case kIOReturnNotFound: return "kIOReturnNotFound";
        case kIOReturnInvalid: return "kIOReturnInvalid";
        default: return "unknown IOReturn";
    }
}
} // namespace ASFW::Logging
