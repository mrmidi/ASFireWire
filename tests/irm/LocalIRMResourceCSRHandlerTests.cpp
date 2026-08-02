// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceCSRHandlerTests.cpp — local OHCI-backed IRM CSR routing.

#include "Bus/IRM/LocalIRMResourceCSRHandler.hpp"

#include "Common/CSRSpace.hpp"
#include "Hardware/IEEE1394.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstring>

namespace {

using ASFW::Async::LocalRequestContext;
using ASFW::Async::ResponseCode;
using ASFW::Bus::LocalIRMResourceCSRHandler;
using ASFW::Driver::HardwareInterface;
using AReq = ASFW::Async::HW::AsyncRequestHeader;
namespace FW = ASFW::FW;

[[nodiscard]] uint64_t CSR(uint32_t offset) noexcept {
    return (0xFFFFull << 32) | offset;
}

[[nodiscard]] std::array<uint8_t, 8> CompareSwapPayload(uint32_t compareValue,
                                                        uint32_t swapValue) noexcept {
    std::array<uint8_t, 8> payload{};
    if constexpr (std::endian::native == std::endian::little) {
        compareValue = std::byteswap(compareValue);
        swapValue = std::byteswap(swapValue);
    }
    const uint32_t compareBE = compareValue;
    const uint32_t swapBE = swapValue;
    std::memcpy(payload.data(), &compareBE, sizeof(compareBE));
    std::memcpy(payload.data() + sizeof(compareBE), &swapBE, sizeof(swapBE));
    return payload;
}

class LocalIRMResourceCSRHandlerTests : public ::testing::Test {
protected:
    void SetUp() override {
        hardware.ResetTestState();
    }

    HardwareInterface hardware;
};

TEST_F(LocalIRMResourceCSRHandlerTests, ReadQuadletUsesHardwareCSRControlValue) {
    LocalIRMResourceCSRHandler handler(&hardware);
    (void)hardware.WriteLocalIRMResource(1, 0x12345678);

    LocalRequestContext ctx{};
    ctx.destOffset = CSR(FW::kCSR_BandwidthAvailable);
    ctx.tCode = AReq::kTcodeReadQuad;

    const auto result = handler.HandleLocalRequest(ctx);
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, ResponseCode::Complete);
    EXPECT_EQ(result.readQuadlet, 0x12345678u);
}

TEST_F(LocalIRMResourceCSRHandlerTests, CompareSwapLockUpdatesMatchingResource) {
    LocalIRMResourceCSRHandler handler(&hardware);
    const auto payload = CompareSwapPayload(0x3Fu, 0x02u);

    LocalRequestContext ctx{};
    ctx.destOffset = CSR(FW::kCSR_BusManagerID);
    ctx.tCode = AReq::kTcodeLockRequest;
    ctx.extendedTCode = static_cast<uint16_t>(FW::LockOp::kCompareSwap);
    ctx.dataLength = 8;
    ctx.writePayload = payload;

    const auto result = handler.HandleLocalRequest(ctx);
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, ResponseCode::Complete);
    EXPECT_EQ(result.lockResponseQuadlet, 0x3Fu);
    EXPECT_EQ(hardware.ReadLocalIRMResource(0).value, 0x02u);
}

TEST_F(LocalIRMResourceCSRHandlerTests, CompareSwapLockReturnsOldValueOnMismatch) {
    LocalIRMResourceCSRHandler handler(&hardware);
    (void)hardware.WriteLocalIRMResource(0, 0x04u);
    const auto payload = CompareSwapPayload(0x3Fu, 0x02u);

    LocalRequestContext ctx{};
    ctx.destOffset = CSR(FW::kCSR_BusManagerID);
    ctx.tCode = AReq::kTcodeLockRequest;
    ctx.extendedTCode = static_cast<uint16_t>(FW::LockOp::kCompareSwap);
    ctx.dataLength = 8;
    ctx.writePayload = payload;

    const auto result = handler.HandleLocalRequest(ctx);
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, ResponseCode::Complete);
    EXPECT_EQ(result.lockResponseQuadlet, 0x04u);
    EXPECT_EQ(hardware.ReadLocalIRMResource(0).value, 0x04u);
}

TEST_F(LocalIRMResourceCSRHandlerTests, UnsupportedIRMResourceTCodeClaimsTypeError) {
    LocalIRMResourceCSRHandler handler(&hardware);

    LocalRequestContext ctx{};
    ctx.destOffset = CSR(FW::kCSR_ChannelsAvailableHi);
    ctx.tCode = AReq::kTcodeWriteQuad;

    const auto result = handler.HandleLocalRequest(ctx);
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, ResponseCode::TypeError);
}

TEST_F(LocalIRMResourceCSRHandlerTests, NonCompareSwapLockClaimsTypeError) {
    LocalIRMResourceCSRHandler handler(&hardware);
    const auto payload = CompareSwapPayload(0x3Fu, 0x02u);

    LocalRequestContext ctx{};
    ctx.destOffset = CSR(FW::kCSR_BusManagerID);
    ctx.tCode = AReq::kTcodeLockRequest;
    ctx.extendedTCode = static_cast<uint16_t>(FW::LockOp::kFetchAdd);
    ctx.dataLength = 8;
    ctx.writePayload = payload;

    const auto result = handler.HandleLocalRequest(ctx);
    EXPECT_TRUE(result.claimed);
    EXPECT_EQ(result.rcode, ResponseCode::TypeError);
}

} // namespace
