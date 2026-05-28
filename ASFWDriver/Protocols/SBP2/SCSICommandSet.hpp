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
    std::vector<uint8_t> payload{};
    std::vector<uint8_t> senseData{};
};

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
