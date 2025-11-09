#pragma once

// Тут мем с богом, слоном и пингвином
// TODO: fixme: make a normal expected header
#if defined(__DRIVERKIT__)
#  include <DriverKit/IOReturn.h>
#  include <DriverKit/IOKit/IOReturn.h>
#  include <DriverKit/IOReturn.h>
  using kr_t = kern_return_t;
#else
#  include <cstdint>
  using kr_t = int32_t;
  // Minimal fallbacks for host builds/tests
  #ifndef kIOReturnSuccess
  constexpr kr_t kIOReturnSuccess       = 0;
  #endif
  #ifndef kIOReturnNotReady
  constexpr kr_t kIOReturnNotReady      = -1;
  #endif
  #ifndef kIOReturnBadArgument
  constexpr kr_t kIOReturnBadArgument   = -2;
  #endif
  #ifndef kIOReturnInternalError
  constexpr kr_t kIOReturnInternalError = -3;
  #endif
#endif
