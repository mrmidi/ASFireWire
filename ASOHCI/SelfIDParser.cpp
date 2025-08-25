// SelfIDParser.cpp
// Self-ID quadlet parsing (IEEE 1394-2008 Alpha §16.3.2.1)

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
    os_log(ASLog(), "ASOHCI: Processing %u Self-ID quadlets (IEEE 1394-2008 Alpha)", quadletCount);
    BRIDGE_LOG("Self-ID processing: %u quads", quadletCount);

    // Helpers per Table 16-4
    auto portCodeStr = [](uint32_t v)->const char* {
        switch (v & 0x3u) {
            case kSelfIDPort_NotPresent: return "none";
            case kSelfIDPort_NotActive:  return "present/idle";
            case kSelfIDPort_Parent:     return "active→parent";
            case kSelfIDPort_Child:      return "active→child";
        }
        return "?";
    };
    auto alphaSpeedStr = [](uint32_t sp)->const char* {
        switch (sp & 0x3u) { case 0: return "S100"; case 1: return "S200"; case 2: return "S400"; default: return "reserved"; }
    };
    auto powerStr = [](uint32_t p)->const char* {
        switch (p & 0x7u) {
            case 0: return "may bus-power, not using";
            case 1: return "≤3W from bus";
            case 2: return "≤7W from bus";
            case 3: return "≤15W from bus";
            case 4: return "self-powered";
            default: return "reserved";
        }
    };

    uint32_t nodes = 0;
    for (uint32_t i = 0; i < quadletCount; ++i) {
        const uint32_t q = selfIDData[i];
        if ((q & kSelfID_Tag_Mask) != kSelfID_Tag_SelfID) {
            os_log(ASLog(), "ASOHCI: Skip non-selfID quadlet[%u]=0x%08x", i, q);
            continue;
        }
        const uint32_t phy = (q & kSelfID_PhyID_Mask) >> kSelfID_PhyID_Shift;
        const bool isExt   = (q & kSelfID_IsExtended_Mask) != 0;
        if (!isExt) {
            // Packet #0 (Alpha)
            const bool     L   = (q & kSelfID_LinkActive_Mask) != 0;
            const uint32_t gap = (q & kSelfID_GapCount_Mask) >> kSelfID_GapCount_Shift;
            const uint32_t sp  = (q & kSelfID_Speed_Mask) >> kSelfID_Speed_Shift;
            const bool     del = (q & kSelfID_Delay_Mask) != 0;
            const bool     c   = (q & kSelfID_Contender_Mask) != 0;
            const uint32_t pwr = (q & kSelfID_PowerClass_Mask) >> 8;
            const uint32_t p0  = (q & kSelfID_P0_Mask) >> 6;
            const uint32_t p1  = (q & kSelfID_P1_Mask) >> 4;
            const uint32_t p2  = (q & kSelfID_P2_Mask) >> 2;
            const bool     ini = (q & kSelfID_Initiated_Mask) != 0;
            const bool     more= (q & kSelfID_More_Mask) != 0;

            os_log(ASLog(), "ASOHCI: Node %u: phy=%u L=%u gap=%u sp=%{public}s del=%u c=%u pwr=%{public}s i=%u m=%u",
                   nodes, phy, L, gap, alphaSpeedStr(sp), del, c, powerStr(pwr), ini, more);
            os_log(ASLog(), "ASOHCI:  ports p0=%{public}s p1=%{public}s p2=%{public}s",
                   portCodeStr(p0), portCodeStr(p1), portCodeStr(p2));
            BRIDGE_LOG("Node%u phy=%u sp=%s L=%u gap=%u c=%u pwr=%u",
                       nodes, phy, alphaSpeedStr(sp), L, gap, c, pwr);

            // Consume optional extended packets (#1/#2) for this phy
            uint8_t portIndex = 3;
            uint32_t j = i + 1;
            while (j < quadletCount) {
                const uint32_t qx = selfIDData[j];
                if ((qx & kSelfID_Tag_Mask) != kSelfID_Tag_SelfID) break;
                const uint32_t phyX = (qx & kSelfID_PhyID_Mask) >> kSelfID_PhyID_Shift;
                if (phyX != phy) break;
                if ((qx & kSelfID_IsExtended_Mask) == 0) break;
                const uint32_t n = (qx & kSelfID_SeqN_Mask) >> kSelfID_SeqN_Shift; // 0 or 1 expected

                uint32_t payload = qx & 0x000FFFFF; // keep low 20 bits for ports
                for (unsigned k = 0; k < 10 && portIndex <= 15; ++k) {
                    const uint32_t code = (payload >> (k*2)) & 0x3u;
                    os_log(ASLog(), "ASOHCI:  port p%u=%{public}s (n=%u)", portIndex, portCodeStr(code), n);
                    ++portIndex;
                }
                ++j;
                if (n == 1) { i = j - 1; break; }
            }
            ++nodes;
        } else {
            const uint32_t n = (q & kSelfID_SeqN_Mask) >> kSelfID_SeqN_Shift;
            os_log(ASLog(), "ASOHCI: Orphan extended self-ID: phy=%u n=%u q=0x%08x", phy, n, q);
        }
    }
    os_log(ASLog(), "ASOHCI: Self-ID parsing complete (nodes=%u)", nodes);
    BRIDGE_LOG("Self-ID done: nodes=%u", nodes);
}

} // namespace SelfIDParser
