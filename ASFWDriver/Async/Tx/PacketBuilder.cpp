#include "PacketBuilder.hpp"

#include <cstring>

#include "../../Hardware/IEEE1394.hpp"
#include "../../Hardware/OHCIConstants.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Async {
namespace {

using HW::AsyncRequestHeader;
using HW::ToBigEndian16;
using HW::ToBigEndian32;

constexpr uint8_t kRetryX = 0b01;
constexpr uint16_t kNodeIDMask = 0xFFFFu;
constexpr uint16_t kNodeNumberMask = 0x3Fu;
constexpr uint16_t kBusNumberMask = 0x03FFu;

// IEEE 1394-1995 §6.2 + OHCI §7.8.1 Figure 7-9: Packet header format
// Quadlet 0: [destination_ID:16][tLabel:6][rt:2][tCode:4][pri:4]
// Quadlet 1: [source_ID:16][destination_offset_high:16]
// Quadlet 2: [destination_offset_low:32]
//
// NOTE: Legacy implementations relied on the controller to patch source_ID
//       from the NodeID register. DriverKit path must program source_ID
//       explicitly because the hardware no longer backfills it.

struct HeaderNoData {
    // Quadlet 0: destination + control bits
    uint16_t destinationID;
    uint8_t  tLabel_rt_tCode_upper;  // [tLabel:6][rt:2]
    uint8_t  tCode_lower_pri;        // [tCode_low:4][pri:4]

    // Quadlet 1: source + offset high
    uint16_t sourceID;               // Local NodeID (bus[15:6] | node[5:0])
    uint16_t destinationOffsetHigh;

    // Quadlet 2: offset low
    uint32_t destinationOffsetLow;
};
static_assert(sizeof(HeaderNoData) == 12);

struct HeaderReadQuadletRequest {
    HeaderNoData base;
    uint32_t reserved;  // OHCI immediate descriptors expect 16 bytes (4 quadlets)
};
static_assert(sizeof(HeaderReadQuadletRequest) == 16);

struct HeaderQuadletData {
    // Quadlet 0: destination + control bits
    uint16_t destinationID;
    uint8_t  tLabel_rt_tCode_upper;
   uint8_t  tCode_lower_pri;

    // Quadlet 1: source + offset high
    uint16_t sourceID;
    uint16_t destinationOffsetHigh;

    // Quadlet 2: offset low
    uint32_t destinationOffsetLow;

    // Quadlet 3: payload data
    uint32_t quadletData;
};
static_assert(sizeof(HeaderQuadletData) == 16);

struct HeaderBlockData {
    // Quadlet 0: destination + control bits
    uint16_t destinationID;
    uint8_t  tLabel_rt_tCode_upper;
    uint8_t  tCode_lower_pri;

    // Quadlet 1: source + offset high
    uint16_t sourceID;
    uint16_t destinationOffsetHigh;

    // Quadlet 2: offset low
    uint32_t destinationOffsetLow;

    // Quadlet 3: data length + extended tCode
    uint16_t dataLength;
    uint16_t extendedTCode;
};
static_assert(sizeof(HeaderBlockData) == 16);

struct HeaderPhyPacket {
    uint32_t tCodeQuadlet;  // Quadlet 0: 0x000000E0 (tCode = 0xE for PHY_PACKET)
    uint32_t dataQuadlet1;  // Quadlet 1: PHY configuration data 1
    uint32_t dataQuadlet2;  // Quadlet 2: PHY configuration data 2
    uint32_t reserved;      // Quadlet 3: Reserved (padding, not transmitted due to reqCount=12)
};
static_assert(sizeof(HeaderPhyPacket) == 16);

[[nodiscard]] bool ValidateAddressHigh(uint32_t addressHigh) {
    return addressHigh <= 0xFFFFu;
}

[[nodiscard]] bool ValidateContext(const PacketContext& context, const char* operation) {
    if ((context.sourceNodeID & kNodeIDMask) == 0) {
        ASFW_LOG(Async,
                 "PacketBuilder::%{public}s: Source NodeID missing (context source=0x%04x)",
                 operation,
                 context.sourceNodeID);
        return false;
    }

    const uint16_t busNumber = static_cast<uint16_t>((context.sourceNodeID >> 6) & kBusNumberMask);
    const uint8_t nodeNumber = static_cast<uint8_t>(context.sourceNodeID & kNodeNumberMask);

    if (context.generation == 0) {
        ASFW_LOG(Async,
                 "PacketBuilder::%{public}s: Bus generation unknown (bus=%u node=%u)",
                 operation,
                 busNumber,
                 nodeNumber);
    }

    return true;
}

[[nodiscard]] uint16_t EncodeSourceNodeID(const PacketContext& context) {
    return static_cast<uint16_t>(context.sourceNodeID & kNodeIDMask);
}

} // namespace

std::size_t PacketBuilder::BuildReadQuadlet(const ReadParams& params,
                                            uint8_t label,
                                            const PacketContext& context,
                                            void* headerBuffer,
                                            std::size_t bufferSize) const {
    // 12 bytes for read-quadlet - NO ADDITIONAL DATA!
    constexpr std::size_t headerSize = sizeof(HeaderNoData);
    if (bufferSize < headerSize || headerBuffer == nullptr) {
        return 0;
    }
    if (params.length != 0 && params.length != 4) {
        return 0;
    }
    if (!ValidateAddressHigh(params.addressHigh)) {
        return 0;
    }
    if (!ValidateContext(context, "BuildReadQuadlet")) {
        return 0;
    }

    auto* header = static_cast<HeaderNoData*>(headerBuffer);
    std::memset(header, 0, headerSize);

    // OHCI INTERNAL AT Data Format (host byte order) - controller converts to wire format
    // CRITICAL FIX: tLabel must be at bits[15:10], NOT bits[23:18]!
    // Per ExtractTLabel in OHCI_HW_Specs.hpp and Apple reference implementation.
    //
    // Quadlet 0: [source_bus_ID:1][reserved:4][spd:3][tl:6][rt:2][tCode:4][reserved:4]
    //            bit[31]           [30:27]     [26:24] [23:18] [9:8] [7:4]   [3:0]
    //                                                   ^^^^^^ WRONG!
    //
    // CORRECT layout (matching ExtractTLabel and Apple):
    // Quadlet 0: [destinationID:16][tl:6][rt:2][tCode:4][pri:4]
    //            bits[31:16]        [15:10][9:8][7:4]  [3:0]
    //
    // Quadlet 1: [sourceID:16][destinationOffsetHigh:16]
    // Quadlet 2: [destinationOffsetLow:32]

    const uint8_t tCode = HW::AsyncRequestHeader::kTcodeReadQuad;
    const uint8_t rt = kRetryX;
    const uint8_t priority = 0;

    label &= 0x3F;
    const uint8_t speedCode = ((params.speedCode != 0xFF) ? params.speedCode : context.speedCode) & 0x07;

    // Build full 16-bit destinationID = (busNumber << 6) | nodeNumber
    const uint16_t srcNodeID = static_cast<uint16_t>(context.sourceNodeID & kNodeIDMask);
    const uint16_t busNumber = static_cast<uint16_t>((srcNodeID >> 6) & kBusNumberMask);
    const uint16_t node      = static_cast<uint16_t>(params.destinationID & kNodeNumberMask);
    const uint16_t destID    = static_cast<uint16_t>((busNumber << 6) | node);

    // Quadlet 0: LINUX OHCI FORMAT (verified from ohci-serdes-test.c)
    // [srcBusID:1][unused:5][speed:3][tLabel:6][retry:2][tCode:4][priority:4]
    // bit[23]      [22:19]   [18:16] [15:10]   [9:8]    [7:4]    [3:0]
    const uint8_t srcBusID = 0;  // Always 0 for local bus
    uint32_t quadlet0 =
        (static_cast<uint32_t>(srcBusID & 0x01) << 23) |  // bit[23]
        (static_cast<uint32_t>(speedCode & 0x07) << 16) | // bits[18:16] - SPEED FIELD!
        (static_cast<uint32_t>(label) << 10) |            // bits[15:10]
        (static_cast<uint32_t>(rt) << 8) |                // bits[9:8]
        (static_cast<uint32_t>(tCode) << 4) |             // bits[7:4]
        (static_cast<uint32_t>(priority) & 0xF);          // bits[3:0]

    // Quadlet 1: LINUX FORMAT - destinationId (NOT sourceId!) + offset high
    // [destinationId:16][destinationOffsetHigh:16]
    const uint32_t quadlet1 =
        (static_cast<uint32_t>(destID) << 16) |           // bits[31:16] - FIXED!
        static_cast<uint32_t>(params.addressHigh & 0xFFFFu);

    // Quadlet 2: offset low
    const uint32_t quadlet2 = params.addressLow;

    // Write exactly 3 quadlets (12 bytes). The immediate buffer has 16 bytes capacity,
    // but DescriptorBuilder will set reqCount=12 — that's the key fix.
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 0, &quadlet0, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 4, &quadlet1, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 8, &quadlet2, 4);

    return headerSize; // 12, not 16
}

std::size_t PacketBuilder::BuildReadBlock(const ReadParams& params,
                                          uint8_t label,
                                          const PacketContext& context,
                                          void* headerBuffer,
                                          std::size_t bufferSize) const {
    constexpr std::size_t headerSize = sizeof(HeaderBlockData);
    if (bufferSize < headerSize || headerBuffer == nullptr) {
        return 0;
    }
    if (params.length == 0 || params.length > 0xFFFFu) {
        return 0;
    }
    if (!ValidateAddressHigh(params.addressHigh)) {
        return 0;
    }
    if (!ValidateContext(context, "BuildReadBlock")) {
        return 0;
    }

    auto* header = static_cast<HeaderBlockData*>(headerBuffer);
    std::memset(header, 0, headerSize);

    // OHCI INTERNAL AT Data Format (host byte order) - controller converts to wire format
    // CRITICAL FIX: tLabel at bits[15:10] to match ExtractTLabel
    const uint8_t tCode = AsyncRequestHeader::kTcodeReadBlock;
    const uint8_t rt = kRetryX;
    const uint8_t priority = 0;

    label &= 0x3F;
    const uint8_t speedCode = ((params.speedCode != 0xFF) ? params.speedCode : context.speedCode) & 0x07;

    // Build full 16-bit destinationID = (busNumber << 6) | nodeNumber
    const uint16_t srcNodeID = static_cast<uint16_t>(context.sourceNodeID & kNodeIDMask);
    const uint16_t busNumber = static_cast<uint16_t>((srcNodeID >> 6) & kBusNumberMask);
    const uint16_t node = static_cast<uint16_t>(params.destinationID & kNodeNumberMask);
    const uint16_t destID = static_cast<uint16_t>((busNumber << 6) | node);

    // Quadlet 0: LINUX OHCI FORMAT (verified from ohci-serdes-test.c)
    // [srcBusID:1][unused:5][speed:3][tLabel:6][retry:2][tCode:4][priority:4]
    const uint8_t srcBusID = 0;  // Always 0 for local bus
    uint32_t quadlet0 =
        (static_cast<uint32_t>(srcBusID & 0x01) << 23) |
        (static_cast<uint32_t>(speedCode & 0x07) << 16) |
        (static_cast<uint32_t>(label) << 10) |
        (static_cast<uint32_t>(rt) << 8) |
        (static_cast<uint32_t>(tCode) << 4) |
        (static_cast<uint32_t>(priority) & 0xF);

    // Quadlet 1: LINUX FORMAT - destinationId + offset high
    const uint32_t quadlet1 =
        (static_cast<uint32_t>(destID) << 16) |
        static_cast<uint32_t>(params.addressHigh & 0xFFFFu);

    // Quadlet 2: offset low
    const uint32_t quadlet2 = params.addressLow;

    // Quadlet 3: dataLength and reserved
    const uint32_t quadlet3 = static_cast<uint32_t>(params.length) << 16;

    // Write all four quadlets in native byte order
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 0, &quadlet0, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 4, &quadlet1, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 8, &quadlet2, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 12, &quadlet3, 4);

    return headerSize;
}

std::size_t PacketBuilder::BuildWriteQuadlet(const WriteParams& params,
                                             uint8_t label,
                                             const PacketContext& context,
                                             void* headerBuffer,
                                             std::size_t bufferSize) const {
    constexpr std::size_t headerSize = sizeof(HeaderQuadletData);
    if (bufferSize < headerSize || headerBuffer == nullptr) {
        return 0;
    }
    if (params.length != 4 || params.payload == nullptr) {
        return 0;
    }
    if (!ValidateAddressHigh(params.addressHigh)) {
        return 0;
    }
    if (!ValidateContext(context, "BuildWriteQuadlet")) {
        return 0;
    }

    auto* header = static_cast<HeaderQuadletData*>(headerBuffer);
    std::memset(header, 0, headerSize);

    // OHCI INTERNAL AT Data Format (host byte order) - controller converts to wire format
    // CRITICAL FIX: tLabel at bits[15:10] to match ExtractTLabel
    const uint8_t tCode = AsyncRequestHeader::kTcodeWriteQuad;
    const uint8_t rt = kRetryX;
    const uint8_t priority = 0;

    label &= 0x3F;
    const uint8_t speedCode = ((params.speedCode != 0xFF) ? params.speedCode : context.speedCode) & 0x07;

    // Build full 16-bit destinationID = (busNumber << 6) | nodeNumber
    const uint16_t srcNodeID = static_cast<uint16_t>(context.sourceNodeID & kNodeIDMask);
    const uint16_t busNumber = static_cast<uint16_t>((srcNodeID >> 6) & kBusNumberMask);
    const uint16_t node = static_cast<uint16_t>(params.destinationID & kNodeNumberMask);
    const uint16_t destID = static_cast<uint16_t>((busNumber << 6) | node);

    // Quadlet 0: LINUX OHCI FORMAT (verified from ohci-serdes-test.c)
    // [srcBusID:1][unused:5][speed:3][tLabel:6][retry:2][tCode:4][priority:4]
    const uint8_t srcBusID = 0;  // Always 0 for local bus
    uint32_t quadlet0 =
        (static_cast<uint32_t>(srcBusID & 0x01) << 23) |
        (static_cast<uint32_t>(speedCode & 0x07) << 16) |
        (static_cast<uint32_t>(label) << 10) |
        (static_cast<uint32_t>(rt) << 8) |
        (static_cast<uint32_t>(tCode) << 4) |
        (static_cast<uint32_t>(priority) & 0xF);

    // Quadlet 1: LINUX FORMAT - destinationId + offset high
    const uint32_t quadlet1 =
        (static_cast<uint32_t>(destID) << 16) |
        static_cast<uint32_t>(params.addressHigh & 0xFFFFu);

    // Quadlet 2: offset low
    const uint32_t quadlet2 = params.addressLow;

    // Quadlet 3: payload data
    uint32_t payloadQuadlet = 0;
    std::memcpy(&payloadQuadlet, params.payload, sizeof(payloadQuadlet));
#if ASFW_SWAP_IMMEDIATE
    // Convert immediate payload to big-endian if hardware doesn't convert it
    payloadQuadlet = ToBigEndian32(payloadQuadlet);
#endif

    // Write all four quadlets in native byte order
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 0, &quadlet0, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 4, &quadlet1, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 8, &quadlet2, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 12, &payloadQuadlet, 4);

    return headerSize;
}

std::size_t PacketBuilder::BuildWriteBlock(const WriteParams& params,
                                           uint8_t label,
                                           const PacketContext& context,
                                           void* headerBuffer,
                                           std::size_t bufferSize) const {
    constexpr std::size_t headerSize = sizeof(HeaderBlockData);
    if (bufferSize < headerSize || headerBuffer == nullptr) {
        return 0;
    }
    if (params.length == 0 || params.length > 0xFFFFu) {
        return 0;
    }
    if (!ValidateAddressHigh(params.addressHigh)) {
        return 0;
    }
    if (!ValidateContext(context, "BuildWriteBlock")) {
        return 0;
    }

    auto* header = static_cast<HeaderBlockData*>(headerBuffer);
    std::memset(header, 0, headerSize);

    // OHCI INTERNAL AT Data Format (host byte order) - controller converts to wire format
    // CRITICAL FIX: tLabel at bits[15:10] to match ExtractTLabel
    const uint8_t tCode = AsyncRequestHeader::kTcodeWriteBlock;
    const uint8_t rt = kRetryX;
    const uint8_t priority = 0;

    label &= 0x3F;
    const uint8_t speedCode = ((params.speedCode != 0xFF) ? params.speedCode : context.speedCode) & 0x07;

    // Build full 16-bit destinationID = (busNumber << 6) | nodeNumber
    const uint16_t srcNodeID = static_cast<uint16_t>(context.sourceNodeID & kNodeIDMask);
    const uint16_t busNumber = static_cast<uint16_t>((srcNodeID >> 6) & kBusNumberMask);
    const uint16_t node = static_cast<uint16_t>(params.destinationID & kNodeNumberMask);
    const uint16_t destID = static_cast<uint16_t>((busNumber << 6) | node);

    // Quadlet 0: LINUX OHCI FORMAT (verified from ohci-serdes-test.c)
    // [srcBusID:1][unused:5][speed:3][tLabel:6][retry:2][tCode:4][priority:4]
    const uint8_t srcBusID = 0;  // Always 0 for local bus
    uint32_t quadlet0 =
        (static_cast<uint32_t>(srcBusID & 0x01) << 23) |
        (static_cast<uint32_t>(speedCode & 0x07) << 16) |
        (static_cast<uint32_t>(label) << 10) |
        (static_cast<uint32_t>(rt) << 8) |
        (static_cast<uint32_t>(tCode) << 4) |
        (static_cast<uint32_t>(priority) & 0xF);

    // Quadlet 1: LINUX FORMAT - destinationId + offset high
    const uint32_t quadlet1 =
        (static_cast<uint32_t>(destID) << 16) |
        static_cast<uint32_t>(params.addressHigh & 0xFFFFu);

    // Quadlet 2: offset low
    const uint32_t quadlet2 = params.addressLow;

    // Quadlet 3: dataLength and reserved
    const uint32_t quadlet3 = static_cast<uint32_t>(params.length) << 16;

    // Write all four quadlets in native byte order
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 0, &quadlet0, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 4, &quadlet1, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 8, &quadlet2, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 12, &quadlet3, 4);

    return headerSize;
}

std::size_t PacketBuilder::BuildLock(const LockParams& params,
                                     uint8_t label,
                                     uint16_t extendedTCode,
                                     const PacketContext& context,
                                     void* headerBuffer,
                                     std::size_t bufferSize) const {
    constexpr std::size_t headerSize = sizeof(HeaderBlockData);
    if (bufferSize < headerSize || headerBuffer == nullptr) {
        return 0;
    }
    if (params.operandLength == 0 || params.operandLength > 0xFFFFu) {
        return 0;
    }
    if ((params.operandLength & 0x3u) != 0) {
        // Operand must be quadlet-aligned per IEEE 1394-1995 §6.2.4.2
        return 0;
    }
    if (!ValidateAddressHigh(params.addressHigh)) {
        return 0;
    }
    if (!ValidateContext(context, "BuildLock")) {
        return 0;
    }

    auto* header = static_cast<HeaderBlockData*>(headerBuffer);
    std::memset(header, 0, headerSize);

    // OHCI INTERNAL AT Data Format (host byte order) - controller converts to wire format
    // CRITICAL FIX: tLabel at bits[15:10] to match ExtractTLabel
    const uint8_t tCode = AsyncRequestHeader::kTcodeLockRequest;
    const uint8_t rt = kRetryX;
    const uint8_t priority = 0;

    label &= 0x3F;
    const uint8_t speedCode = ((params.speedCode != 0xFF) ? params.speedCode : context.speedCode) & 0x07;

    // Build full 16-bit destinationID = (busNumber << 6) | nodeNumber
    const uint16_t srcNodeID = static_cast<uint16_t>(context.sourceNodeID & kNodeIDMask);
    const uint16_t busNumber = static_cast<uint16_t>((srcNodeID >> 6) & kBusNumberMask);
    const uint16_t node = static_cast<uint16_t>(params.destinationID & kNodeNumberMask);
    const uint16_t destID = static_cast<uint16_t>((busNumber << 6) | node);

    // Quadlet 0: LINUX OHCI FORMAT (verified from ohci-serdes-test.c)
    // [srcBusID:1][unused:5][speed:3][tLabel:6][retry:2][tCode:4][priority:4]
    const uint8_t srcBusID = 0;  // Always 0 for local bus
    uint32_t quadlet0 =
        (static_cast<uint32_t>(srcBusID & 0x01) << 23) |
        (static_cast<uint32_t>(speedCode & 0x07) << 16) |
        (static_cast<uint32_t>(label) << 10) |
        (static_cast<uint32_t>(rt) << 8) |
        (static_cast<uint32_t>(tCode) << 4) |
        (static_cast<uint32_t>(priority) & 0xF);

    // Quadlet 1: LINUX FORMAT - destinationId + offset high
    const uint32_t quadlet1 =
        (static_cast<uint32_t>(destID) << 16) |
        static_cast<uint32_t>(params.addressHigh & 0xFFFFu);

    // Quadlet 2: offset low
    const uint32_t quadlet2 = params.addressLow;

    // Quadlet 3: dataLength (in bytes) and extendedTcode
    const uint32_t quadlet3 =
        (static_cast<uint32_t>(params.operandLength) << 16) |
        static_cast<uint32_t>(extendedTCode & 0xFFFFu);

    // Write all four quadlets in native byte order
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 0, &quadlet0, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 4, &quadlet1, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 8, &quadlet2, 4);
    std::memcpy(reinterpret_cast<uint8_t*>(header) + 12, &quadlet3, 4);

    return headerSize;
}

std::size_t PacketBuilder::BuildPhyPacket(const PhyParams& params,
                                          void* headerBuffer,
                                          std::size_t bufferSize) const {
    // PHY packet: 12 bytes transmitted (3 quadlets) per OHCI §7.8.1.4 Figure 7-14
    // Apple's implementation: reqCount=12 (not 16!)
    // Quadlets: [0]=tCode 0xE0, [1]=data1, [2]=data2
    // The 4th quadlet is reserved padding (not transmitted)
    //
    // CRITICAL: PHY packets in immediate descriptors use WIRE FORMAT (big-endian bytes),
    // NOT OHCI internal format. The descriptor data is transmitted as-is on the wire.
    // On little-endian systems, writing uint32_t 0x000000E0 to memory gives bytes [0xE0, 0x00, 0x00, 0x00].
    constexpr std::size_t kPhyPacketSize = 12;  // 3 quadlets only
    constexpr std::size_t headerSize = sizeof(HeaderPhyPacket);

    if (headerBuffer == nullptr || bufferSize < headerSize) {
        return 0;
    }

    // Use byte pointer for direct big-endian wire format construction
    auto* bytes = static_cast<uint8_t*>(headerBuffer);

    // Quadlet 0: tCode = 0xE in bits [7:4] → wire bytes [0xE0, 0x00, 0x00, 0x00]
    // On little-endian system, uint32_t 0x000000E0 stored in memory = [0xE0, 0x00, 0x00, 0x00]
    const uint32_t tCodeQuadlet = 0x000000E0u;
    std::memcpy(bytes + 0, &tCodeQuadlet, 4);

    // Quadlets 1-2: PHY configuration data in big-endian wire format
    // params.quadlet1/2 are already in the format expected on the wire (big-endian)
    // So convert them to wire format using ToBigEndian32
    const uint32_t data1Wire = ToBigEndian32(params.quadlet1);
    const uint32_t data2Wire = ToBigEndian32(params.quadlet2);
    std::memcpy(bytes + 4, &data1Wire, 4);
    std::memcpy(bytes + 8, &data2Wire, 4);

    // Quadlet 3: Reserved (padding, not transmitted - omitted from copy by returning 12)
    const uint32_t reserved = 0;
    std::memcpy(bytes + 12, &reserved, 4);

    // Return 12 bytes (3 quadlets) - DescriptorBuilder will set reqCount=12
    // Only the first 12 bytes will be copied to immediate descriptor and transmitted
    return kPhyPacketSize;
}

} // namespace ASFW::Async
