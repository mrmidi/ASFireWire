// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCExchangeLogWireTests.cpp - The paged FCP exchange log the app reads.

#include <gtest/gtest.h>

#include "ASFWDriver/UserClient/WireFormats/AVCExchangeLogWire.hpp"

#include <cstring>
#include <vector>

namespace {

using ASFW::Protocols::AVC::FcpExchangeLog;
using ASFW::Protocols::AVC::FcpExchangeOutcome;
using ASFW::Protocols::AVC::FcpExchangeRecord;
using ASFW::UserClient::Wire::AVCExchangePageWire;
using ASFW::UserClient::Wire::AVCExchangeRecordWire;
using ASFW::UserClient::Wire::SerializeExchangePage;

FcpExchangeLog MakeLog() {
    FcpExchangeLog log{.session = 3, .dropped = 2};
    log.records.push_back(FcpExchangeRecord{
        .sequence = 7, .generation = 4, .outcome = FcpExchangeOutcome::kResponse, .interim = true,
        .retries = 1, .command = {0x01, 0xFF, 0x30, 0xFF}, .response = {0x0C, 0xFF, 0x30}});
    log.records.push_back(FcpExchangeRecord{
        .sequence = 8, .generation = 4, .outcome = FcpExchangeOutcome::kTimeout,
        .command = {0x01, 0xFF, 0x31, 0x07, 0xFF}});
    return log;
}

template <typename T>
T ReadAt(const std::vector<uint8_t>& bytes, size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

TEST(AVCExchangeLogWire, PageCarriesSessionDropsAndEachRecordsBytes) {
    const auto bytes = SerializeExchangePage(MakeLog(), 0, 4096);
    const auto page = ReadAt<AVCExchangePageWire>(bytes, 0);
    EXPECT_EQ(page.session, 3U);
    EXPECT_EQ(page.dropped, 2U);
    EXPECT_EQ(page.totalRecords, 2U);
    EXPECT_EQ(page.firstIndex, 0U);
    ASSERT_EQ(page.recordCount, 2U);

    size_t offset = sizeof(AVCExchangePageWire);
    const auto first = ReadAt<AVCExchangeRecordWire>(bytes, offset);
    EXPECT_EQ(first.sequence, 7U);
    EXPECT_EQ(first.outcome, static_cast<uint8_t>(FcpExchangeOutcome::kResponse));
    EXPECT_EQ(first.interim, 1U);
    EXPECT_EQ(first.retries, 1U);
    ASSERT_EQ(first.commandLength, 4U);
    ASSERT_EQ(first.responseLength, 3U);
    offset += sizeof(AVCExchangeRecordWire);
    EXPECT_EQ(std::vector<uint8_t>(bytes.begin() + offset, bytes.begin() + offset + 7),
              (std::vector<uint8_t>{0x01, 0xFF, 0x30, 0xFF, 0x0C, 0xFF, 0x30}));
    offset += 8;  // 7 bytes padded to 8

    const auto second = ReadAt<AVCExchangeRecordWire>(bytes, offset);
    EXPECT_EQ(second.outcome, static_cast<uint8_t>(FcpExchangeOutcome::kTimeout));
    EXPECT_EQ(second.commandLength, 5U);
    EXPECT_EQ(second.responseLength, 0U);
    EXPECT_EQ(bytes.size(), offset + sizeof(AVCExchangeRecordWire) + 8);
}

TEST(AVCExchangeLogWire, PageStopsBeforeTheBudgetAndResumesFromAnIndex) {
    const auto log = MakeLog();
    // Room for the header and the first record (16 + 8) only.
    const auto first = SerializeExchangePage(log, 0, sizeof(AVCExchangePageWire) + 24);
    EXPECT_EQ(ReadAt<AVCExchangePageWire>(first, 0).recordCount, 1U);

    const auto rest = SerializeExchangePage(log, 1, 4096);
    const auto page = ReadAt<AVCExchangePageWire>(rest, 0);
    EXPECT_EQ(page.firstIndex, 1U);
    ASSERT_EQ(page.recordCount, 1U);
    EXPECT_EQ(ReadAt<AVCExchangeRecordWire>(rest, sizeof(AVCExchangePageWire)).sequence, 8U);

    const auto past = SerializeExchangePage(log, 5, 4096);
    EXPECT_EQ(ReadAt<AVCExchangePageWire>(past, 0).recordCount, 0U);
    EXPECT_EQ(past.size(), sizeof(AVCExchangePageWire));
}

} // namespace
