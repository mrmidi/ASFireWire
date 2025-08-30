// SelfIDParser.cpp
// Self-ID quadlet parsing and buffer analysis
//
// References:
// - IEEE 1394-2008 Alpha §16.3.2.1 (Self-ID packet format and PHY ID
// assignment)
// - IEEE 1394-2008 Annex P (Deriving bus topology from self-ID packets)
// - OHCI 1.1 §11.3 (Self-ID receive format and buffer structure)

#include "SelfIDParser.hpp"

#ifndef ASOHCI_VERBOSE_SELFID
#define ASOHCI_VERBOSE_SELFID 0
#endif

#include <DriverKit/IOLib.h>
#include <os/log.h>

#include "LogHelper.hpp"
#include "OHCIConstants.hpp"

namespace SelfIDParser {

void Process(uint32_t *selfIDData, uint32_t quadletCount) {
  if (!selfIDData || quadletCount == 0) {
    os_log(ASLog(), "ASOHCI: Invalid Self-ID data");
    return;
  }
  os_log(ASLog(),
         "ASOHCI: Processing %u Self-ID quadlets (IEEE 1394-2008 Alpha)",
         quadletCount);

// OHCI 1.1 §11.3: Buffer format = [header quadlet][concatenated self-ID packet
// data] Header quadlet (position 0): selfIDGeneration | timeStamp (NOT tagged)
// Data quadlets: Self-ID packets (tag=10b) + inverted check quadlets + topology
// maps
#if ASOHCI_VERBOSE_SELFID
  os_log(ASLog(),
         "ASOHCI: === RAW SELF-ID BUFFER ANALYSIS (OHCI 1.1 §11.3) ===");
  for (uint32_t i = 0; i < quadletCount; ++i) {
    uint32_t q = selfIDData[i];
    uint32_t tag = (q >> 30) & 0x3;
    const char *tagType = "";
    const char *purpose = "";
    if (i == 0) {
      purpose = " [HEADER: generation | timestamp]";
      tagType = "N/A-Header";
    } else {
      switch (tag) {
      case 0:
        tagType = "00b-Reserved";
        purpose = " [Unknown/Reserved]";
        break;
      case 1:
        tagType = "01b-Topology";
        purpose = " [Topology Map]";
        break;
      case 2:
        tagType = "10b-SelfID";
        purpose = " [Self-ID Packet]";
        break;
      case 3:
        tagType = "11b-Reserved";
        purpose = " [Inverted Check?]";
        break;
      }
    }
    os_log(ASLog(), "ASOHCI:  BUF[%u]=0x%08x tag=%s%s", i, q, tagType, purpose);
  }
  os_log(ASLog(), "ASOHCI: === END BUFFER ANALYSIS ===");
#endif

  // Helpers per Table 16-4
  auto portCodeStr = [](uint32_t v) -> const char * {
    switch (v & 0x3u) {
    case kSelfIDPort_NotPresent:
      return "none";
    case kSelfIDPort_NotActive:
      return "present/idle";
    case kSelfIDPort_Parent:
      return "active→parent";
    case kSelfIDPort_Child:
      return "active→child";
    }
    return "?";
  };
  auto alphaSpeedStr = [](uint32_t sp) -> const char * {
    switch (sp & 0x3u) {
    case 0:
      return "S100";
    case 1:
      return "S200";
    case 2:
      return "S400";
    default:
      return "reserved";
    }
  };
  auto powerStr = [](uint32_t p) -> const char * {
    switch (p & 0x7u) {
    case 0:
      return "may bus-power, not using";
    case 1:
      return "≤3W from bus";
    case 2:
      return "≤7W from bus";
    case 3:
      return "≤15W from bus";
    case 4:
      return "self-powered";
    default:
      return "reserved";
    }
  };

  // IEEE 1394-2008 Annex P + OHCI 1.1 §11.3: Collect ALL Self-ID packets
  // (tag=10b) Buffer contains mixed data: header + Self-ID packets + inverted
  // checks + topology maps Skip buffer[0] (header quadlet) and scan remaining
  // data for tagged Self-ID packets
  uint32_t
      selfIDIndices[32]; // max 32 Self-ID packets should be more than enough
  uint32_t selfIDCount = 0;

  for (uint32_t i = 1; i < quadletCount && selfIDCount < 32;
       ++i) { // Start from 1 (skip header)
    if ((selfIDData[i] & kSelfID_Tag_Mask) == kSelfID_Tag_SelfID) {
      selfIDIndices[selfIDCount] = i;
      selfIDCount++;
    }
  }

  if (selfIDCount == 0) {
    os_log(ASLog(),
           "ASOHCI: No tagged Self-ID packets found in %u data quadlets (OHCI "
           "buffer corruption?)",
           quadletCount - 1);
    return;
  }

// Log all discovered Self-ID packets with their buffer positions
#if ASOHCI_VERBOSE_SELFID
  os_log(ASLog(), "ASOHCI: === SELF-ID PACKET DISCOVERY ===");
  os_log(ASLog(), "ASOHCI: Found %u Self-ID packets in %u total quadlets",
         selfIDCount, quadletCount);
  for (uint32_t i = 0; i < selfIDCount; ++i) {
    uint32_t idx = selfIDIndices[i];
    uint32_t packet = selfIDData[idx];
    uint32_t phy = (packet & kSelfID_PhyID_Mask) >> kSelfID_PhyID_Shift;
    os_log(ASLog(), "ASOHCI:  SelfID[%u]: buffer[%u]=0x%08x (PHY %u)", i, idx,
           packet, phy);
  }
  os_log(ASLog(), "ASOHCI: === END DISCOVERY ===");
#endif

// OHCI 1.1 §11.3: "Host Controller does not verify the integrity of the self-ID
// packets and software is responsible for performing this function (i.e., using
// the logical inverse quadlet)." Check for inverted quadlets following each
// Self-ID packet for error detection
#if ASOHCI_VERBOSE_SELFID
  os_log(ASLog(),
         "ASOHCI: === INVERTED QUADLET VERIFICATION (OHCI 1.1 §11.3) ===");
  for (uint32_t i = 0; i < selfIDCount; ++i) {
    uint32_t selfIDIdx = selfIDIndices[i];
    uint32_t selfIDPacket = selfIDData[selfIDIdx];

    // Look for inverted check quadlet immediately after Self-ID packet
    if (selfIDIdx + 1 < quadletCount) {
      uint32_t nextQuad = selfIDData[selfIDIdx + 1];
      uint32_t expectedInverse = ~selfIDPacket;

      if (nextQuad == expectedInverse) {
        os_log(ASLog(),
               "ASOHCI:  SelfID[%u]: Inverted check PASSED (0x%08x ^ 0x%08x)",
               i, selfIDPacket, nextQuad);
      } else {
        // Check if next quadlet has no tag (might be inverted check with
        // different position)
        uint32_t nextTag = (nextQuad >> 30) & 0x3;
        if (nextTag == 3) { // 11b tag might indicate inverted data
          os_log(ASLog(),
                 "ASOHCI:  SelfID[%u]: Potential inverted check at +1: 0x%08x "
                 "(tag=11b)",
                 i, nextQuad);
        } else {
          os_log(ASLog(),
                 "ASOHCI:  SelfID[%u]: No inverted check found at +1 "
                 "(next=0x%08x, expected=0x%08x)",
                 i, nextQuad, expectedInverse);
        }
      }
    } else {
      os_log(ASLog(),
             "ASOHCI:  SelfID[%u]: No space for inverted check (end of buffer)",
             i);
    }
  }
  os_log(ASLog(), "ASOHCI: === END VERIFICATION ===");
#endif

  uint32_t nodes = 0;
  for (uint32_t sidIdx = 0; sidIdx < selfIDCount; ++sidIdx) {
    const uint32_t bufIdx = selfIDIndices[sidIdx];
    const uint32_t q = selfIDData[bufIdx];
    const uint32_t phy = (q & kSelfID_PhyID_Mask) >> kSelfID_PhyID_Shift;
    const bool isExt = (q & kSelfID_IsExtended_Mask) != 0;
    if (!isExt) {
      // Packet #0 (Alpha)
      const bool L = (q & kSelfID_LinkActive_Mask) != 0;
      const uint32_t gap =
          (q & kSelfID_GapCount_Mask) >> kSelfID_GapCount_Shift;
      const uint32_t sp = (q & kSelfID_Speed_Mask) >> kSelfID_Speed_Shift;
      const bool del = (q & kSelfID_Delay_Mask) != 0;
      const bool c = (q & kSelfID_Contender_Mask) != 0;
      const uint32_t pwr = (q & kSelfID_PowerClass_Mask) >> 8;
      const uint32_t p0 = (q & kSelfID_P0_Mask) >> 6;
      const uint32_t p1 = (q & kSelfID_P1_Mask) >> 4;
      const uint32_t p2 = (q & kSelfID_P2_Mask) >> 2;
      const bool ini = (q & kSelfID_Initiated_Mask) != 0;
      const bool more = (q & kSelfID_More_Mask) != 0;

      os_log(ASLog(),
             "ASOHCI: Node %u: phy=%u L=%u gap=%u sp=%{public}s del=%u c=%u "
             "pwr=%{public}s i=%u m=%u",
             nodes, phy, L, gap, alphaSpeedStr(sp), del, c, powerStr(pwr), ini,
             more);
      os_log(ASLog(),
             "ASOHCI:  ports p0=%{public}s p1=%{public}s p2=%{public}s",
             portCodeStr(p0), portCodeStr(p1), portCodeStr(p2));

      // Look for optional extended packets (#1/#2) for this phy in remaining
      // Self-ID packets
      uint8_t portIndex = 3;
      for (uint32_t extIdx = sidIdx + 1; extIdx < selfIDCount; ++extIdx) {
        const uint32_t extBufIdx = selfIDIndices[extIdx];
        const uint32_t qx = selfIDData[extBufIdx];
        const uint32_t phyX = (qx & kSelfID_PhyID_Mask) >> kSelfID_PhyID_Shift;
        if (phyX != phy)
          continue; // different PHY - keep looking
        if ((qx & kSelfID_IsExtended_Mask) == 0)
          continue; // not extended

        const uint32_t n =
            (qx & kSelfID_SeqN_Mask) >> kSelfID_SeqN_Shift; // 0 or 1 expected
        os_log(ASLog(), "ASOHCI:  Found extended packet n=%u for phy=%u", n,
               phy);

        uint32_t payload = qx & 0x000FFFFF; // keep low 20 bits for ports
        for (unsigned k = 0; k < 10 && portIndex <= 15; ++k) {
          const uint32_t code = (payload >> (k * 2)) & 0x3u;
          os_log(ASLog(), "ASOHCI:  port p%u=%{public}s (n=%u)", portIndex,
                 portCodeStr(code), n);
          ++portIndex;
        }
        if (n == 1)
          break; // packet #2 processed, done with extensions
      }
      ++nodes;
    } else {
      const uint32_t n = (q & kSelfID_SeqN_Mask) >> kSelfID_SeqN_Shift;
      os_log(ASLog(), "ASOHCI: Orphan extended self-ID: phy=%u n=%u q=0x%08x",
             phy, n, q);
    }
  }
  os_log(ASLog(), "ASOHCI: Self-ID parsing complete (nodes=%u)", nodes);
}

} // namespace SelfIDParser
