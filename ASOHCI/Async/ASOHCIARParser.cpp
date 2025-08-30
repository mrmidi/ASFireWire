//
// ASOHCIARParser.cpp
// ASOHCI
//
// Minimal IEEE‑1394 async frame parser for AR path (DriverKit‑friendly, RAII)
//

#include "ASOHCIARParser.hpp"
#include "ASOHCIARTypes.hpp"

#include <DriverKit/IOReturn.h>
#include <stdint.h>

namespace {
static inline uint16_t bswap16(uint16_t v) {
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap16(v);
#else
  return (uint16_t)((v << 8) | (v >> 8));
#endif
}

static inline uint32_t bswap32(uint32_t v) {
#if defined(__clang__) || defined(__GNUC__)
  return __builtin_bswap32(v);
#else
  return (v << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) |
         (v >> 24);
#endif
}

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline uint32_t be32_to_cpu(uint32_t v) { return v; }
static inline uint16_t be16_to_cpu(uint16_t v) { return v; }
#else
static inline uint32_t be32_to_cpu(uint32_t v) { return bswap32(v); }
static inline uint16_t be16_to_cpu(uint16_t v) { return bswap16(v); }
#endif

static inline ARTCode DecodeARTCode(uint32_t hdr1_host) {
  uint8_t nib = static_cast<uint8_t>((hdr1_host >> 28) & 0xF);
  switch (nib) {
  case 0x0:
    return ARTCode::kWriteQuadlet;
  case 0x1:
    return ARTCode::kWriteBlock;
  case 0x2:
    return ARTCode::kWriteResponse;
  case 0x4:
    return ARTCode::kReadQuadlet;
  case 0x5:
    return ARTCode::kReadBlock;
  case 0x6:
    return ARTCode::kReadResponse;
  case 0x0C:
    return ARTCode::kCycleStart; // some silicon reports cycle start
  case 0x07:
    return ARTCode::kLock; // lock family
  default:
    return ARTCode::kUnknown;
  }
}

static inline bool IsReq(ARTCode tc) {
  switch (tc) {
  case ARTCode::kWriteQuadlet:
  case ARTCode::kWriteBlock:
  case ARTCode::kReadQuadlet:
  case ARTCode::kReadBlock:
    return true;
  default:
    return false;
  }
}
} // namespace

uint32_t ASOHCIARParser::HeaderSize(const uint8_t * /*bytes*/,
                                    uint32_t /*len*/) const {
  // Callers normally prefer Parse() which infers size from tCode.
  // Keep this as a simple utility if needed later.
  return 8;
}

bool ASOHCIARParser::Parse(const ARPacketView &view,
                           ARParsedPacket *out) const {
  if (!out)
    return false;
  *out = ARParsedPacket{};
  if (!view.data || view.length < 8)
    return false;

  const uint8_t *p = view.data;
  uint32_t h0 = be32_to_cpu(*reinterpret_cast<const uint32_t *>(p + 0));
  uint32_t h1 = be32_to_cpu(*reinterpret_cast<const uint32_t *>(p + 4));

  ARTCode tc = DecodeARTCode(h1);
  out->tcode = tc;
  out->isRequest = IsReq(tc);

  // Infer header size based on common async formats
  uint32_t hdrBytes = 8;
  switch (tc) {
  case ARTCode::kWriteBlock:
  case ARTCode::kReadBlock:
  case ARTCode::kLock:
    hdrBytes = 16;
    break;
  default:
    hdrBytes = 8;
    break;
  }
  if (view.length < hdrBytes)
    hdrBytes = 8;
  out->headerBytes = hdrBytes;

  // Populate payload slice
  if (view.length > hdrBytes) {
    out->payload = p + hdrBytes;
    out->payloadBytes = view.length - hdrBytes;
  } else {
    out->payload = nullptr;
    out->payloadBytes = 0;
  }

  // Optional fields (src/dst/address) left as zeros for now
  (void)h0; // reserved for future header decoding
  return true;
}
