// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCExchangeLogWire.hpp - One unit's FCP exchange log, paged for the user client.
//
// Page layout (all little-endian, the app's native order):
//   AVCExchangePageWire
//   recordCount x { AVCExchangeRecordWire, command bytes, response bytes,
//                   zero padding to a 4-byte boundary }
// The app asks again from firstIndex + recordCount until it has totalRecords.

#pragma once

#include "../../Protocols/AVC/FcpExchangeRecorder.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ASFW::UserClient::Wire {

struct AVCExchangePageWire {
    uint32_t session;       ///< Exchange session (attach = 1, +1 per refresh)
    uint32_t dropped;       ///< Exchanges the session had no room for
    uint32_t totalRecords;  ///< Records in the whole log
    uint32_t firstIndex;    ///< Index of this page's first record
    uint32_t recordCount;   ///< Records in this page
    uint32_t _reserved;
} __attribute__((packed));

static_assert(sizeof(AVCExchangePageWire) == 24, "AVCExchangePageWire must be 24 bytes");

struct AVCExchangeRecordWire {
    uint32_t sequence;        ///< Transport-wide exchange number
    uint32_t generation;      ///< Bus generation when the exchange ended
    uint8_t outcome;          ///< Protocols::AVC::FcpExchangeOutcome
    uint8_t interim;          ///< 1 if an INTERIM preceded the final response
    uint8_t retries;          ///< Resends before the final outcome
    uint8_t _reserved;
    uint16_t commandLength;   ///< Command bytes that follow
    uint16_t responseLength;  ///< Response bytes after the command (0 = none)
} __attribute__((packed));

static_assert(sizeof(AVCExchangeRecordWire) == 16, "AVCExchangeRecordWire must be 16 bytes");

/// Serialise records from `firstIndex` until the next would exceed `maxBytes`.
[[nodiscard]] inline std::vector<uint8_t> SerializeExchangePage(
    const Protocols::AVC::FcpExchangeLog& log, uint32_t firstIndex, size_t maxBytes) {
    std::vector<uint8_t> out(sizeof(AVCExchangePageWire));
    AVCExchangePageWire page{
        .session = log.session,
        .dropped = log.dropped,
        .totalRecords = static_cast<uint32_t>(log.records.size()),
        .firstIndex = firstIndex,
        .recordCount = 0,
        ._reserved = 0,
    };
    for (size_t i = firstIndex; i < log.records.size(); ++i) {
        const auto& record = log.records[i];
        const size_t body = record.command.size() + record.response.size();
        const size_t padded = (sizeof(AVCExchangeRecordWire) + body + 3U) & ~size_t{3};
        if (out.size() + padded > maxBytes) {
            break;
        }
        const AVCExchangeRecordWire header{
            .sequence = record.sequence,
            .generation = record.generation,
            .outcome = static_cast<uint8_t>(record.outcome),
            .interim = record.interim ? uint8_t{1} : uint8_t{0},
            .retries = record.retries,
            ._reserved = 0,
            .commandLength = static_cast<uint16_t>(record.command.size()),
            .responseLength = static_cast<uint16_t>(record.response.size()),
        };
        const size_t offset = out.size();
        out.resize(offset + padded, 0);
        std::memcpy(out.data() + offset, &header, sizeof(header));
        if (!record.command.empty()) {
            std::memcpy(out.data() + offset + sizeof(header), record.command.data(),
                        record.command.size());
        }
        if (!record.response.empty()) {
            std::memcpy(out.data() + offset + sizeof(header) + record.command.size(),
                        record.response.data(), record.response.size());
        }
        ++page.recordCount;
    }
    std::memcpy(out.data(), &page, sizeof(page));
    return out;
}

} // namespace ASFW::UserClient::Wire
