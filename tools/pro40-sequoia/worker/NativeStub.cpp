// SPDX-License-Identifier: Apache-2.0
// Test-only native boundary: no hardware API is linked. Open fails normally,
// or exits3 when explicitly selected to test missing-result containment.
#include "../NativeFireWire.h"
#include <IOKit/IOReturn.h>
#include <cstdio>
#include <cstdlib>
extern "C" {
int32_t pro40_open(uint64_t) {
    std::puts("test-native-open: no hardware");std::fflush(stdout);
    if(std::getenv("PRO40_WORKER_TEST_FATAL"))std::_Exit(3);
    return kIOReturnNotFound;
}
void pro40_close(){std::puts("test-native-close: no hardware");}
int32_t pro40_state(uint32_t *,uint16_t *){std::abort();}
int32_t pro40_admit_read(uint64_t,uint32_t){std::abort();}
int32_t pro40_read(uint32_t,uint64_t,uint32_t,void *){std::abort();}
void pro40_begin_cleanup(){std::abort();}
int32_t pro40_clock_state(uint32_t,Pro40ClockState *){std::abort();}
void *pro40_native_interface(){std::abort();}
}
