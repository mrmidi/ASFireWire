#include <gtest/gtest.h>

#include "ASFWDriver/Async/Core/Error.hpp"
#include "ASFWDriver/Common/FWCommon.hpp"

namespace {

TEST(FireWireIOReturnTests, QueuePendingAndResponsePendingRemainDistinct) {
    EXPECT_NE(ASFW::FW::kASFWIOReturnPendingQueue, ASFW::FW::kASFWIOReturnResponsePending);
    EXPECT_TRUE(ASFW::FW::IsFireWireIOReturn(ASFW::FW::kASFWIOReturnPendingQueue));
    EXPECT_TRUE(ASFW::FW::IsFireWireIOReturn(ASFW::FW::kASFWIOReturnResponsePending));
}

TEST(FireWireIOReturnTests, ResponseMappingPreservesFireWireFamilyEncoding) {
    EXPECT_EQ(ASFW::FW::MapRespToIOReturn(ASFW::FW::Response::ConflictError),
              ASFW::FW::kASFWIOReturnResponseConflict);
    EXPECT_EQ(ASFW::FW::MapRespToIOReturn(ASFW::FW::Response::DataError),
              ASFW::FW::kASFWIOReturnResponseDataError);
    EXPECT_EQ(ASFW::FW::MapRespToIOReturn(ASFW::FW::Response::TypeError),
              ASFW::FW::kASFWIOReturnResponseTypeError);
    EXPECT_EQ(ASFW::FW::MapRespToIOReturn(ASFW::FW::Response::AddressError),
              ASFW::FW::kASFWIOReturnResponseAddressError);
    EXPECT_EQ(ASFW::FW::MapRespToIOReturn(ASFW::FW::Response::BusReset),
              ASFW::FW::kASFWIOReturnBusReset);
    EXPECT_EQ(ASFW::FW::MapRespToIOReturn(ASFW::FW::Response::Pending),
              ASFW::FW::kASFWIOReturnResponsePending);
}

TEST(FireWireIOReturnTests, AckMappingUsesReservedASFWBlockForAckFailures) {
    EXPECT_EQ(ASFW::FW::MapAckToIOReturn(ASFW::FW::Ack::BusyX), ASFW::FW::kASFWIOReturnAckBusy);
    EXPECT_EQ(ASFW::FW::MapAckToIOReturn(ASFW::FW::Ack::BusyA), ASFW::FW::kASFWIOReturnAckBusy);
    EXPECT_EQ(ASFW::FW::MapAckToIOReturn(ASFW::FW::Ack::BusyB), ASFW::FW::kASFWIOReturnAckBusy);
    EXPECT_EQ(ASFW::FW::MapAckToIOReturn(ASFW::FW::Ack::TypeError),
              ASFW::FW::kASFWIOReturnAckTypeError);
    EXPECT_EQ(ASFW::FW::MapAckToIOReturn(ASFW::FW::Ack::DataError),
              ASFW::FW::kASFWIOReturnAckDataError);
    EXPECT_EQ(ASFW::FW::MapAckToIOReturn(ASFW::FW::Ack::Timeout), kIOReturnTimeout);
}

TEST(FireWireIOReturnTests, AsyncErrorClassifiesBoundaryStatusWithoutLosingIOReturn) {
    const auto result =
        ASFW::Async::ToResult(ASFW::FW::kASFWIOReturnAckBusy, "remote node reported busy");

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ASFW::Async::ErrorCode::FireWire);
    EXPECT_EQ(result.error().BoundaryStatus(), ASFW::FW::kASFWIOReturnAckBusy);
    EXPECT_EQ(ASFW::Async::ToKernReturn(result), ASFW::FW::kASFWIOReturnAckBusy);
}

} // namespace
