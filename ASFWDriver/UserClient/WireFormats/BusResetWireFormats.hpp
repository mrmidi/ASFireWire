//
//  BusResetWireFormats.hpp
//  ASFWDriver
//
//  Wire format structures for bus reset packet capture
//

#ifndef ASFW_USERCLIENT_BUS_RESET_WIRE_FORMATS_HPP
#define ASFW_USERCLIENT_BUS_RESET_WIRE_FORMATS_HPP

#include "WireFormatsCommon.hpp"

namespace ASFW::UserClient::Wire {

struct __attribute__((packed)) BusResetPacketWire {
    uint64_t captureTimestamp;
    uint32_t generation;
    uint8_t eventCode;
    uint8_t tCode;
    uint16_t cycleTime;
    uint32_t rawQuadlets[4];
    uint32_t wireQuadlets[4];
    char contextInfo[64];
};

} // namespace ASFW::UserClient::Wire

#endif // ASFW_USERCLIENT_BUS_RESET_WIRE_FORMATS_HPP
