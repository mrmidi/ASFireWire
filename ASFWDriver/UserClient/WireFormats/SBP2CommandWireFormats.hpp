#pragma once

#include <cstddef>
#include <cstdint>

namespace ASFW::UserClient::Wire {

inline constexpr uint32_t kSBP2CommandMaxCDBLength = 16;
inline constexpr uint32_t kSBP2CommandMaxTransferLength = 16u * 1024u * 1024u;

// DriverKit user-client ABI records. These are native-endian host records,
// not SBP-2 big-endian bus wire structures.
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

static_assert(sizeof(SBP2CommandRequestWire) == 20);
static_assert(offsetof(SBP2CommandRequestWire, cdbLength) == 0);
static_assert(offsetof(SBP2CommandRequestWire, transferLength) == 4);
static_assert(offsetof(SBP2CommandRequestWire, outgoingLength) == 8);
static_assert(offsetof(SBP2CommandRequestWire, timeoutMs) == 12);
static_assert(offsetof(SBP2CommandRequestWire, direction) == 16);
static_assert(offsetof(SBP2CommandRequestWire, captureSenseData) == 17);
static_assert(offsetof(SBP2CommandRequestWire, _reserved) == 18);

static_assert(sizeof(SBP2CommandResultWire) == 16);
static_assert(offsetof(SBP2CommandResultWire, transportStatus) == 0);
static_assert(offsetof(SBP2CommandResultWire, sbpStatus) == 4);
static_assert(offsetof(SBP2CommandResultWire, _reserved) == 5);
static_assert(offsetof(SBP2CommandResultWire, payloadLength) == 8);
static_assert(offsetof(SBP2CommandResultWire, senseLength) == 12);

} // namespace ASFW::UserClient::Wire
