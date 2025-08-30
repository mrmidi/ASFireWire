// SelfIDDecode.cpp
// Implementation of pure IEEE 1394 Alpha Self-ID decode (no DriverKit deps)

#include "SelfIDDecode.hpp"

#include <algorithm>
#include <stddef.h>

// We may reuse bit definitions from OHCIConstants.hpp without DK deps
#include "OHCIConstants.hpp"

namespace SelfID {

static inline PortCode toPortCode(uint32_t twoBits) {
  switch (twoBits & 0x3u) {
  case 0:
    return PortCode::NotPresent;
  case 1:
    return PortCode::NotActive;
  case 2:
    return PortCode::Parent;
  case 3:
    return PortCode::Child;
  }
  return PortCode::NotPresent;
}

Result Decode(const uint32_t *buffer, uint32_t quadletCount) {
  Result out{};
  if (!buffer || quadletCount == 0) {
    out.integrityOk = false;
    out.warnings.push_back({"Empty or null buffer"});
    return out;
  }

  // OHCI 1.1 §11.3: buffer[0] is header (generation|timestamp). Generation
  // often matches SelfIDCount bits [23:16].
  out.generation = (quadletCount > 0) ? ((buffer[0] >> 16) & 0xFFu) : 0;

  // DEBUG: Log buffer contents (removed for DriverKit compatibility)
  // printf("SelfID::Decode: buffer[0]=0x%08x gen=%u, quadletCount=%u\n",
  // buffer[0], out.generation, quadletCount);
  for (uint32_t i = 1; i < quadletCount && i < 16; ++i) {
    uint32_t tag = (buffer[i] >> 30) & 0x3;
    // printf("SelfID::Decode: buffer[%u]=0x%08x tag=%u\n", i, buffer[i], tag);
  }

  // Discover all tagged Self-ID packets (tag=10b) starting from index 1.
  // Also attempt a minimal inverse check (next quadlet equals bitwise not).
  bool integrityOk = true;
  std::vector<uint32_t> sidIdx;
  sidIdx.reserve(32);

  for (uint32_t i = 1; i < quadletCount; ++i) {
    uint32_t q = buffer[i];
    uint32_t tag = (q >> 30) & 0x3;
    if ((q & kSelfID_Tag_Mask) == kSelfID_Tag_SelfID) {
      // printf("SelfID::Decode: Found Self-ID packet at index %u: 0x%08x\n", i,
      // q);
      sidIdx.push_back(i);
      if (i + 1 < quadletCount) {
        uint32_t inv = buffer[i + 1];
        if ((inv != ~q) && (((inv >> 30) & 0x3u) != 3u)) {
          integrityOk = false; // missing strict inverse and not tagged as 11b
          // printf("SelfID::Decode: Integrity check failed for packet at %u\n",
          // i);
        }
      }
    }
  }

  out.integrityOk = integrityOk;
  // printf("SelfID::Decode: Found %zu Self-ID packets, integrityOk=%d\n",
  // sidIdx.size(), integrityOk ? 1 : 0);

  if (sidIdx.empty()) {
    out.warnings.push_back({"No Self-ID packets (tag=10b) found"});
    return out;
  }

  // Decode base and extended packets by PHY id
  // Build a simple map phyId -> partial AlphaRecord (first occurrence wins for
  // base packet)
  struct Partial {
    AlphaRecord rec;
    bool haveBase = false;
    uint8_t nextPort = 3;
  };
  Partial tmp[64]{}; // PHY ids limited to 0..63

  for (size_t n = 0; n < sidIdx.size(); ++n) {
    uint32_t idx = sidIdx[n];
    uint32_t q = buffer[idx];
    uint32_t phy = (q & kSelfID_PhyID_Mask) >> kSelfID_PhyID_Shift;
    bool isExt = (q & kSelfID_IsExtended_Mask) != 0;

    Partial &p = tmp[phy & 63u];

    if (!isExt) {
      // Base alpha packet
      p.haveBase = true;
      p.rec.phyId = static_cast<uint8_t>(phy);
      p.rec.linkActive = (q & kSelfID_LinkActive_Mask) != 0;
      p.rec.gapCount = static_cast<uint8_t>((q & kSelfID_GapCount_Mask) >>
                                            kSelfID_GapCount_Shift);
      p.rec.speed = static_cast<LinkSpeed>((q & kSelfID_Speed_Mask) >>
                                           kSelfID_Speed_Shift);
      p.rec.delay = (q & kSelfID_Delay_Mask) != 0;
      p.rec.contender = (q & kSelfID_Contender_Mask) != 0;
      p.rec.powerClass =
          static_cast<uint8_t>((q & kSelfID_PowerClass_Mask) >> 8);
      p.rec.initiated = (q & kSelfID_Initiated_Mask) != 0;
      p.rec.more = (q & kSelfID_More_Mask) != 0;

      // three inline ports
      p.rec.ports[0] = toPortCode((q & kSelfID_P0_Mask) >> 6);
      p.rec.ports[1] = toPortCode((q & kSelfID_P1_Mask) >> 4);
      p.rec.ports[2] = toPortCode((q & kSelfID_P2_Mask) >> 2);
      p.nextPort = 3;
    } else {
      // Extended packets: 10 two-bit port codes per packet
      if (!p.haveBase) {
        // Orphan extension observed. Record a warning once per PHY.
        out.warnings.push_back(
            {"Orphan extended Self-ID for PHY " + std::to_string(phy)});
      }
      uint32_t payload = q & 0x000FFFFFu;
      for (unsigned k = 0; k < 10 && p.nextPort < 16; ++k) {
        uint32_t code = (payload >> (k * 2)) & 0x3u;
        p.rec.ports[p.nextPort++] = toPortCode(code);
      }
    }
  }

  // Emit results in PHY order 0..63 as encountered
  for (unsigned phy = 0; phy < 64; ++phy) {
    if (tmp[phy].haveBase)
      out.nodes.push_back(tmp[phy].rec);
  }

  return out;
}

} // namespace SelfID
