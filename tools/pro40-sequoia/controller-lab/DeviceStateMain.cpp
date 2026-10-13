// SPDX-License-Identifier: Apache-2.0
#include "../NativeFireWire.h"
#include <cstdio>
#include <cstring>
#ifndef PRO40_MANAGED_REAL_GUID
#error A device-state helper requires one build-time device GUID.
#endif
int main(int argc,const char **argv) {
    if(argc!=2 || std::strcmp(argv[1],"--current"))return 64;
    // Host-local observation only: IOFireWireLibDevice.cpp:1033-1046 calls
    // kGetGenerationAndNodeID; no DICE read/write or isochronous allocation.
    // Reuse the existing exact GUID/vendor/model match and Open/Close contract.
    auto status=pro40_open(PRO40_MANAGED_REAL_GUID);
    uint32_t generation=0;uint16_t node=0;
    if(!status)status=pro40_state(&generation,&node);
    pro40_close();
    if(status || (node&63)==63) {
        std::fprintf(stderr,"device-state-unavailable:%08x\n",static_cast<unsigned>(status));return 69;
    }
    if(std::printf("PRO40_DEVICE_STATE 1 %016llx %u %u\n",static_cast<unsigned long long>(PRO40_MANAGED_REAL_GUID),generation,node)<0 || std::fflush(stdout))return 74;
    return 0;
}
