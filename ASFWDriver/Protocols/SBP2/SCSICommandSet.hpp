#pragma once

#include "SBP2WireFormats.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ASFW::Protocols::SBP2::SCSI {

enum class DataDirection : uint8_t {
    None = 0,
    FromTarget = 1,
    ToTarget = 2,
};

struct CommandRequest {
    std::vector<uint8_t> cdb{};
    DataDirection direction{DataDirection::None};
    uint32_t transferLength{0};
    std::vector<uint8_t> outgoingPayload{};
    uint32_t timeoutMs{2000};
    bool captureSenseData{false};

    [[nodiscard]] bool HasTransferBuffer() const noexcept {
        return transferLength > 0;
    }
};

struct CommandResult {
    int transportStatus{0};
    uint8_t sbpStatus{Wire::SBPStatus::kNoAdditionalInfo};
    // SAM status from the status block's command-set-dependent bytes (SBP-2
    // Annex B). Without this a CHECK CONDITION (0x02) looks like GOOD because
    // only sbpStatus is surfaced. Valid only when the target sent
    // command-set-dependent status (it may omit it on GOOD).
    uint8_t scsiStatus{0};
    bool scsiStatusValid{false};
    std::vector<uint8_t> payload{};
    std::vector<uint8_t> senseData{};
};

// SBP-2 Annex B packs autosense into the status block's command-set-dependent
// bytes (status block bytes 8+, here data[0..]). Convert to a standard 18-byte
// fixed-format sense block, applying the bit transform from Linux
// firewire/sbp2.c sbp2_status_to_sense_data() (sbp2.c:1295,1303-1305) — the
// two layouts do not line up byte-for-byte:
//   data[0]  [7:6] sfmt (0 current, 1 deferred, 2/3 undecodable), [5:0] SAM status
//   data[1]  [7] valid, [6] mark, [5] eom, [4] ili, [3:0] sense key
//   data[2]  ASC          data[3]  ASCQ
//   data[4..7]   information
//   data[8..11]  command-set-specific information
//   data[12..13] FRU + sense-key-dependent
// sfmt selects the fixed-format response code (0x70 current / 0x71 deferred),
// data[1]'s valid bit moves to sense[0] bit7, and its mark/eom/ili bits shift
// up one into sense[2] [7:5] while the key stays in [3:0].
// Returns empty when there is nothing to decode (no data, deferred-only or
// vendor sfmt).
inline std::vector<uint8_t> ConvertSBP2StatusToSenseData(std::span<const uint8_t> data) {
    if (data.size() < 14) {
        return {};
    }
    const uint8_t sfmt = (data[0] >> 6) & 0x3;
    if (sfmt == 2 || sfmt == 3) {
        return {};  // dequeued/vendor-dependent format — cannot decode
    }
    std::vector<uint8_t> sense(18, 0);
    sense[0] = 0x70 | sfmt | (data[1] & 0x80);
    sense[2] = ((data[1] << 1) & 0xe0) | (data[1] & 0x0f);
    sense[3] = data[4];
    sense[4] = data[5];
    sense[5] = data[6];
    sense[6] = data[7];
    sense[7] = 10;          // additional sense length
    sense[8] = data[8];
    sense[9] = data[9];
    sense[10] = data[10];
    sense[11] = data[11];
    sense[12] = data[2];    // ASC
    sense[13] = data[3];    // ASCQ
    sense[14] = data[12];
    sense[15] = data[13];
    return sense;
}

inline CommandRequest BuildRawCDBRequest(std::span<const uint8_t> cdb,
                                        DataDirection direction,
                                        uint32_t transferLength = 0,
                                        std::span<const uint8_t> outgoingPayload = {},
                                        uint32_t timeoutMs = 2000,
                                        bool captureSenseData = false) {
    CommandRequest request{};
    request.cdb.assign(cdb.begin(), cdb.end());
    request.direction = direction;
    request.transferLength = transferLength;
    request.outgoingPayload.assign(outgoingPayload.begin(), outgoingPayload.end());
    request.timeoutMs = timeoutMs;
    request.captureSenseData = captureSenseData;
    return request;
}

inline CommandRequest BuildInquiryRequest(uint8_t allocationLength = 96) {
    return BuildRawCDBRequest(
        std::array<uint8_t, 6>{0x12, 0x00, 0x00, allocationLength, 0x00, 0x00},
        DataDirection::FromTarget,
        allocationLength);
}

inline CommandRequest BuildTestUnitReadyRequest() {
    return BuildRawCDBRequest(
        std::array<uint8_t, 6>{0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        DataDirection::None,
        0);
}

inline CommandRequest BuildRequestSenseRequest(uint8_t allocationLength = 18) {
    return BuildRawCDBRequest(
        std::array<uint8_t, 6>{0x03, 0x00, 0x00, allocationLength, 0x00, 0x00},
        DataDirection::FromTarget,
        allocationLength,
        {},
        2000,
        true);
}

} // namespace ASFW::Protocols::SBP2::SCSI
