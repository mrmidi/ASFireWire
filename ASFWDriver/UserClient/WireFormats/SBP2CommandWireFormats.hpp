#pragma once

#include <cstdint>

namespace ASFW::UserClient::Wire {

struct __attribute__((packed)) SBP2CommandRequestWire {
    uint32_t cdbLength;
    uint32_t transferLength;
    uint32_t outgoingLength;
    uint32_t timeoutMs;
    uint8_t direction;
    uint8_t captureSenseData;
    uint8_t _reserved[2];
    // Followed by: CDB bytes, then outgoing payload bytes.
};

struct __attribute__((packed)) SBP2CommandResultWire {
    int32_t transportStatus;
    uint8_t sbpStatus;
    uint8_t _reserved[3];
    uint32_t payloadLength;
    uint32_t senseLength;
    // Followed by: payload bytes, then sense bytes.
};

} // namespace ASFW::UserClient::Wire
