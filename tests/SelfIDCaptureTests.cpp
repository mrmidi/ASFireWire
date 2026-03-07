#include <gtest/gtest.h>

#include "ASFWDriver/Bus/SelfIDCapture.hpp"
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/RegisterMap.hpp"

namespace ASFW::Driver {

class SelfIDCaptureTestPeer {
  public:
    static uint32_t* MutableQuadlets(SelfIDCapture& capture) {
        return reinterpret_cast<uint32_t*>(capture.map_->GetAddress());
    }
};

} // namespace ASFW::Driver

using namespace ASFW::Driver;

namespace {

uint32_t MakeBaseSelfID(uint8_t phyId, uint8_t gapCount) {
    uint32_t quadlet = 0x80000000U;
    quadlet |= (static_cast<uint32_t>(phyId) & 0x3FU) << 24U;
    quadlet |= 1U << 22U;
    quadlet |= (static_cast<uint32_t>(gapCount) & 0x3FU) << 16U;
    quadlet |= 0x2U << 14U;
    return quadlet;
}

uint32_t MakeSelfIDCountRegister(uint8_t generation, uint32_t quadletCount) {
    return (static_cast<uint32_t>(generation) << SelfIDCountBits::kGenerationShift) |
           (quadletCount << SelfIDCountBits::kSizeShift);
}

} // namespace

TEST(SelfIDCaptureTests, ValidInversePairsAreNormalized) {
    HardwareInterface hardware;
    SelfIDCapture capture;

    ASSERT_EQ(capture.PrepareBuffers(8, hardware), kIOReturnSuccess);
    ASSERT_EQ(capture.Arm(hardware), kIOReturnSuccess);

    auto* quadlets = SelfIDCaptureTestPeer::MutableQuadlets(capture);
    const uint32_t node0 = MakeBaseSelfID(0U, 63U);
    const uint32_t node1 = MakeBaseSelfID(1U, 63U);
    quadlets[0] = 0x002A0000U;
    quadlets[1] = node0;
    quadlets[2] = ~node0;
    quadlets[3] = node1;
    quadlets[4] = ~node1;

    const uint32_t countRegister = MakeSelfIDCountRegister(0x2AU, 5U);
    hardware.SetTestRegister(Register32::kSelfIDCount, countRegister);

    auto result = capture.Decode(countRegister, hardware);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->generation, 0x2AU);
    ASSERT_EQ(result->quads.size(), 3U);
    EXPECT_EQ(result->quads[1], node0);
    EXPECT_EQ(result->quads[2], node1);
    ASSERT_EQ(result->sequences.size(), 2U);
    const auto expectedFirst = std::pair<size_t, unsigned int>{1U, 1U};
    const auto expectedSecond = std::pair<size_t, unsigned int>{2U, 1U};
    EXPECT_EQ(result->sequences[0], expectedFirst);
    EXPECT_EQ(result->sequences[1], expectedSecond);
}

TEST(SelfIDCaptureTests, InvalidInversePairIsRejected) {
    HardwareInterface hardware;
    SelfIDCapture capture;

    ASSERT_EQ(capture.PrepareBuffers(8, hardware), kIOReturnSuccess);
    ASSERT_EQ(capture.Arm(hardware), kIOReturnSuccess);

    auto* quadlets = SelfIDCaptureTestPeer::MutableQuadlets(capture);
    const uint32_t node0 = MakeBaseSelfID(0U, 63U);
    quadlets[0] = 0x002A0000U;
    quadlets[1] = node0;
    quadlets[2] = 0xDEADBEEFU;

    const uint32_t countRegister = MakeSelfIDCountRegister(0x2AU, 3U);
    hardware.SetTestRegister(Register32::kSelfIDCount, countRegister);

    auto result = capture.Decode(countRegister, hardware);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, SelfIDCapture::DecodeErrorCode::InvalidInversePair);
}

TEST(SelfIDCaptureTests, GenerationMismatchIsRejected) {
    HardwareInterface hardware;
    SelfIDCapture capture;

    ASSERT_EQ(capture.PrepareBuffers(8, hardware), kIOReturnSuccess);
    ASSERT_EQ(capture.Arm(hardware), kIOReturnSuccess);

    auto* quadlets = SelfIDCaptureTestPeer::MutableQuadlets(capture);
    const uint32_t node0 = MakeBaseSelfID(0U, 63U);
    quadlets[0] = 0x002A0000U;
    quadlets[1] = node0;
    quadlets[2] = ~node0;

    const uint32_t countRegister = MakeSelfIDCountRegister(0x29U, 3U);
    hardware.SetTestRegister(Register32::kSelfIDCount, countRegister);

    auto result = capture.Decode(countRegister, hardware);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, SelfIDCapture::DecodeErrorCode::GenerationMismatch);
}
