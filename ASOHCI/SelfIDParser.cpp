// SelfIDParser.cpp
// Self-ID quadlet parsing and debug logging

#include "SelfIDParser.hpp"

#include <os/log.h>
#include <DriverKit/IOLib.h>

#include "OHCIConstants.hpp"
#include "BridgeLog.hpp"
#include "LogHelper.hpp"

namespace SelfIDParser {

void Process(uint32_t* selfIDData, uint32_t quadletCount)
{
    if (!selfIDData || quadletCount == 0) {
        os_log(ASLog(), "ASOHCI: Invalid Self-ID data");
        return;
    }
    os_log(ASLog(), "ASOHCI: Processing %u Self-ID quadlets", quadletCount);
    BRIDGE_LOG("Self-ID processing: %u quads", quadletCount);

    uint32_t nodeCount = 0;
    for (uint32_t i = 0; i < quadletCount; i++) {
        uint32_t q = selfIDData[i];
        if ((q & 0x1) == 0) {
            uint32_t physID    = (q & kSelfID_PhyID_Mask)      >> kSelfID_PhyID_Shift;
            bool     linkAct   = (q & kSelfID_LinkActive_Mask)  != 0;
            uint32_t gap       = (q & kSelfID_GapCount_Mask)    >> kSelfID_GapCount_Shift;
            uint32_t speed     = (q & kSelfID_Speed_Mask)       >> kSelfID_Speed_Shift;
            bool     contender = (q & kSelfID_Contender_Mask)   != 0;
            uint32_t pwrClass  = (q & kSelfID_PowerClass_Mask)  >> 8;

            const char *speedStr[] = {"S100","S200","S400","S800"};
            const char *spd = (speed < 4) ? speedStr[speed] : "Unknown";

            os_log(ASLog(), "ASOHCI: Node %u: PhyID=%u Link=%d Gap=%u Speed=%s Contender=%d Power=%u",
                  nodeCount, physID, linkAct, gap, spd, contender, pwrClass);
            BRIDGE_LOG("Node%u: PhyID=%u Link=%d Gap=%u Speed=%s",
                       nodeCount, physID, linkAct, gap, spd);
            nodeCount++;
        } else {
            os_log(ASLog(), "ASOHCI: Non-Self-ID quadlet[%u]=0x%08x", i, q);
        }
    }
    os_log(ASLog(), "ASOHCI: Self-ID processing complete: %u nodes discovered", nodeCount);
    BRIDGE_LOG("Self-ID done: %u nodes", nodeCount);
}

} // namespace SelfIDParser
