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

TEST(AddressSpaceManagerTests, ApplyRemoteWriteAcceptsQuadletAlignedMisalignedSource) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);

    uint64_t handle = 0;
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(reinterpret_cast<void*>(0xD),
                                           0xFFFF,
                                           0x0021'0000,
                                           16,
                                           &handle,
                                           nullptr));

    alignas(8) std::array<uint8_t, 20> raw{};
    raw[4] = 0x10;
    raw[5] = 0x20;
    raw[6] = 0x30;
    raw[7] = 0x40;
    raw[8] = 0x50;
    raw[9] = 0x60;
    raw[10] = 0x70;
    raw[11] = 0x80;

    const uint64_t writeAddress = ComposeAddress(0xFFFF, 0x0021'0000) + 4;
    const auto payload = std::span<const uint8_t>(raw.data() + 4, 8);
    ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(payload.data()) & 0x3u);
    ASSERT_EQ(4u, reinterpret_cast<uintptr_t>(payload.data()) & 0x7u);

    EXPECT_EQ(ASFW::Async::ResponseCode::Complete,
              manager.ApplyRemoteWrite(writeAddress, payload));

    std::vector<uint8_t> readback;
    ASSERT_EQ(kIOReturnSuccess,
              manager.ReadIncomingData(reinterpret_cast<void*>(0xD),
                                       handle,
                                       0,
                                       16,
                                       &readback));

    ASSERT_EQ(16u, readback.size());
    EXPECT_EQ(0x10, readback[4]);
    EXPECT_EQ(0x20, readback[5]);
    EXPECT_EQ(0x30, readback[6]);
    EXPECT_EQ(0x40, readback[7]);
    EXPECT_EQ(0x50, readback[8]);
    EXPECT_EQ(0x60, readback[9]);
    EXPECT_EQ(0x70, readback[10]);
    EXPECT_EQ(0x80, readback[11]);
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

TEST(AddressSpaceManagerTests, AutoAllocationReturnsDistinctAlignedRanges) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);

    uint64_t firstHandle = 0;
    ASFW::Protocols::SBP2::AddressSpaceManager::AddressRangeMeta firstMeta{};
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(reinterpret_cast<void*>(0x5),
                                               0xFFFF,
                                               16,
                                               &firstHandle,
                                               &firstMeta));

    uint64_t secondHandle = 0;
    ASFW::Protocols::SBP2::AddressSpaceManager::AddressRangeMeta secondMeta{};
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(reinterpret_cast<void*>(0x6),
                                               0xFFFF,
                                               24,
                                               &secondHandle,
                                               &secondMeta));

    EXPECT_EQ(0xFFFFu, firstMeta.addressHi);
    EXPECT_EQ(0xFFFFu, secondMeta.addressHi);
    EXPECT_EQ(0u, firstMeta.addressLo % 8u);
    EXPECT_EQ(0u, secondMeta.addressLo % 8u);
    EXPECT_LT(firstMeta.addressLo, secondMeta.addressLo);
    EXPECT_LE(firstMeta.addressLo + firstMeta.length, secondMeta.addressLo);
}

TEST(AddressSpaceManagerTests, AutoAllocationSkipsOccupiedFixedRange) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);

    uint64_t fixedHandle = 0;
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRange(reinterpret_cast<void*>(0x7),
                                           0xFFFF,
                                           0x0010'0000,
                                           16,
                                           &fixedHandle,
                                           nullptr));

    uint64_t autoHandle = 0;
    ASFW::Protocols::SBP2::AddressSpaceManager::AddressRangeMeta autoMeta{};
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(reinterpret_cast<void*>(0x8),
                                               0xFFFF,
                                               16,
                                               &autoHandle,
                                               &autoMeta));

    EXPECT_EQ(0x0010'0010u, autoMeta.addressLo);
}

TEST(AddressSpaceManagerTests, AutoAllocationReusesFreedGap) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);

    uint64_t firstHandle = 0;
    ASFW::Protocols::SBP2::AddressSpaceManager::AddressRangeMeta firstMeta{};
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(reinterpret_cast<void*>(0x9),
                                               0xFFFF,
                                               16,
                                               &firstHandle,
                                               &firstMeta));

    uint64_t secondHandle = 0;
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(reinterpret_cast<void*>(0xA),
                                               0xFFFF,
                                               16,
                                               &secondHandle,
                                               nullptr));

    ASSERT_EQ(kIOReturnSuccess,
              manager.DeallocateAddressRange(reinterpret_cast<void*>(0x9), firstHandle));

    uint64_t thirdHandle = 0;
    ASFW::Protocols::SBP2::AddressSpaceManager::AddressRangeMeta thirdMeta{};
    ASSERT_EQ(kIOReturnSuccess,
              manager.AllocateAddressRangeAuto(reinterpret_cast<void*>(0xB),
                                               0xFFFF,
                                               8,
                                               &thirdHandle,
                                               &thirdMeta));

    EXPECT_EQ(firstMeta.addressLo, thirdMeta.addressLo);
}

TEST(AddressSpaceManagerTests, AutoAllocationRejectsRequestLargerThanWindow) {
    ASFW::Protocols::SBP2::AddressSpaceManager manager(nullptr);

    uint64_t handle = 0;
    EXPECT_EQ(kIOReturnNoSpace,
              manager.AllocateAddressRangeAuto(reinterpret_cast<void*>(0xC),
                                               0xFFFF,
                                               0x0FF0'0001u,
                                               &handle,
                                               nullptr));
}
