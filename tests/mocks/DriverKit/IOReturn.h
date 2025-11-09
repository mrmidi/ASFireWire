// DriverKit/IOReturn.h stub for host testing
#pragma once

#include <cstdint>

// kern_return_t and IOReturn types
typedef int kern_return_t;
typedef kern_return_t IOReturn;

// IOReturn constants
#define kIOReturnSuccess             0
#define kIOReturnError               (-1)
#define kIOReturnNoMemory            (-2)
#define kIOReturnNoResources         (-3)
#define kIOReturnBadArgument         (-4)
#define kIOReturnBusy                (-5)
#define kIOReturnTimeout             (-6)
#define kIOReturnNotReady            (-7)
#define kIOReturnNoSpace             (-8)
#define kIOReturnNotAttached         (-9)
#define kIOReturnExclusiveAccess     (-10)
#define kIOReturnIOError             (-11)
#define kIOReturnNotWritable         (-12)
#define kIOReturnNotAligned          (-13)
#define kIOReturnBadMedia            (-14)
#define kIOReturnCannotLock          (-15)
