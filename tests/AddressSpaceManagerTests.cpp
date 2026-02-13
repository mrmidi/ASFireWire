#include <gtest/gtest.h>
#include <array>

#include "ASFWDriver/Protocols/SBP2/AddressSpaceManager.hpp"

namespace {

uint64_t ComposeAddress(uint16_t hi, uint32_t lo) {
    return (static_cast<uint64_t>(hi) << 32) | static_cast<uint64_t>(lo);
}

} // namespace

TEST(AddressSpaceManagerTests, AllocateWriteReadRoundTrip) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);
    ASSERT_TRUE(manager.IsReady());

    uint64_t handle = 0;
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(reinterpret_cast<void*>(0x1),
                                           0xFFFF,
                                           0x0010'0000,
                                           16,
                                           &handle,
                                           nullptr));

    const std::array<uint8_t, 4> payload{0x11, 0x22, 0x33, 0x44};
    EXPECT_EQ(kIOReturnSuccess,
              manager.WriteLocalData(reinterpret_cast<void*>(0x1),
                                     handle,
                                     4,
                                     std::span<const uint8_t>(payload.data(), payload.size())));

    std::vector<uint8_t> readback;
    ASSERT_EQ(kIOReturnSuccess,
              manager.ReadIncomingData(reinterpret_cast<void*>(0x1),
                                       handle,
                                       0,
                                       16,
                                       &readback));

    ASSERT_EQ(16u, readback.size());
    EXPECT_EQ(0x11, readback[4]);
    EXPECT_EQ(0x22, readback[5]);
    EXPECT_EQ(0x33, readback[6]);
    EXPECT_EQ(0x44, readback[7]);
}

TEST(AddressSpaceManagerTests, ApplyRemoteWriteThenReadIncoming) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);

    uint64_t handle = 0;
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(reinterpret_cast<void*>(0x2),
                                           0xFFFF,
                                           0x0020'0000,
                                           12,
                                           &handle,
                                           nullptr));

    const uint64_t writeAddress = ComposeAddress(0xFFFF, 0x0020'0000) + 2;
    const std::array<uint8_t, 3> payload{0xAA, 0xBB, 0xCC};
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              manager.ApplyRemoteWrite(
                  writeAddress,
                  std::span<const uint8_t>(payload.data(), payload.size())));

    std::vector<uint8_t> readback;
    ASSERT_EQ(kIOReturnSuccess,
              manager.ReadIncomingData(reinterpret_cast<void*>(0x2),
                                       handle,
                                       0,
                                       12,
                                       &readback));

    ASSERT_EQ(12u, readback.size());
    EXPECT_EQ(0xAA, readback[2]);
    EXPECT_EQ(0xBB, readback[3]);
    EXPECT_EQ(0xCC, readback[4]);
}

TEST(AddressSpaceManagerTests, ReadAfterDeallocateReturnsNotFound) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);

    uint64_t handle = 0;
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(reinterpret_cast<void*>(0x3),
                                           0xFFFF,
                                           0x0030'0000,
                                           8,
                                           &handle,
                                           nullptr));

    ASSERT_EQ(kIOReturnSuccess,
              manager.DeallocateAddressRange(reinterpret_cast<void*>(0x3), handle));

    std::vector<uint8_t> readback;
    EXPECT_EQ(kIOReturnNotFound,
              manager.ReadIncomingData(reinterpret_cast<void*>(0x3),
                                       handle,
                                       0,
                                       4,
                                       &readback));
}

TEST(AddressSpaceManagerTests, OutOfBoundsReadReturnsNoSpace) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);

    uint64_t handle = 0;
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(reinterpret_cast<void*>(0x4),
                                           0xFFFF,
                                           0x0040'0000,
                                           8,
                                           &handle,
                                           nullptr));

    std::vector<uint8_t> readback;
    EXPECT_EQ(kIOReturnNoSpace,
              manager.ReadIncomingData(reinterpret_cast<void*>(0x4),
                                       handle,
                                       6,
                                       4,
                                       &readback));
}
