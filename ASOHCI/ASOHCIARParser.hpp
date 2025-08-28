#pragma once
//
// ASOHCIARParser.hpp
// Minimal IEEE-1394 async packet header parsing for AR path
//
// Spec refs: OHCI 1.1 §8.7 (AR data formats), IEEE 1394 async packet headers (tCode, length)

#include <DriverKit/DriverKit.h>
#include <stdint.h>
#include "ASOHCIARTypes.hpp"

struct ARParsedPacket {
    bool           isRequest = true;    // request vs response (derived from tCode/category)
    ARTCode        tcode = ARTCode::kUnknown;
    uint16_t       srcNodeID = 0;
    uint16_t       destNodeID = 0;
    uint64_t       address = 0;         // CSR / memory addr if applicable
    const uint8_t* payload = nullptr;
    uint32_t       payloadBytes = 0;
    uint32_t       headerBytes = 0;     // 8/12/16, per format
};

class ASOHCIARParser {
public:
    ASOHCIARParser() = default;
    ~ASOHCIARParser() = default;

    kern_return_t Initialize() { return kIOReturnSuccess; }

    // Returns false if the buffer doesn’t contain a full AR async frame
    bool Parse(const ARPacketView& view, ARParsedPacket* out) const;

    // Utility: discover IEEE-1394 header size of a frame (2/3/4 quadlets)
    uint32_t HeaderSize(const uint8_t* bytes, uint32_t len) const;
};

