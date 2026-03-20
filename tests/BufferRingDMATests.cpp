#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "ASFWDriver/Async/Rx/ARPacketParser.hpp"
#include "ASFWDriver/Common/DMASafeCopy.hpp"
#include "ASFWDriver/Hardware/OHCIDescriptors.hpp"
#include "ASFWDriver/Shared/Rings/BufferRing.hpp"
#include "ASFWDriver/Testing/FakeDMAMemory.hpp"

namespace ASFW::Testing {

namespace {

std::vector<uint8_t> MakeARDmaBufferFromHostWords(std::initializer_list<uint32_t> quadlets,
                                                  uint32_t trailerLE = 0) {
    std::vector<uint8_t> bytes;
    bytes.reserve(quadlets.size() * sizeof(uint32_t) + sizeof(uint32_t));

    for (uint32_t word : quadlets) {
        bytes.push_back(static_cast<uint8_t>(word & 0xFF));
        bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
        bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
    }

    bytes.push_back(static_cast<uint8_t>(trailerLE & 0xFF));
    bytes.push_back(static_cast<uint8_t>((trailerLE >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((trailerLE >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((trailerLE >> 24) & 0xFF));

    return bytes;
}

} // namespace

class BufferRingDMATest : public ::testing::Test {
protected:
    FakeDMAMemory dma_{512 * 1024};
    Shared::BufferRing ring_{};
    uint64_t descBaseIOVA_{0};
    uint64_t bufBaseIOVA_{0};

    void SetUp() override {
        constexpr size_t kNum = 32;
        constexpr size_t kBufSize = 256;

        auto descRegion = dma_.AllocateRegion(kNum * sizeof(Async::HW::OHCIDescriptor));
        ASSERT_TRUE(descRegion.has_value());
        descBaseIOVA_ = descRegion->deviceBase;

        auto bufRegion = dma_.AllocateRegion(kNum * kBufSize);
        ASSERT_TRUE(bufRegion.has_value());
        bufBaseIOVA_ = bufRegion->deviceBase;

        auto* descs = reinterpret_cast<Async::HW::OHCIDescriptor*>(descRegion->virtualBase);
        std::span<Async::HW::OHCIDescriptor> descSpan{descs, kNum};
        std::span<uint8_t> bufSpan{bufRegion->virtualBase, kNum * kBufSize};

        ASSERT_TRUE(ring_.Initialize(descSpan, bufSpan, kNum, kBufSize));
        ring_.BindDma(&dma_);
        ASSERT_TRUE(ring_.Finalize(descBaseIOVA_, bufBaseIOVA_));
    }
};

class LargeBufferRingDMATest : public ::testing::Test {
protected:
    static constexpr size_t kNumBuffers = 4;
    static constexpr size_t kBufferSize = 4160;

    FakeDMAMemory dma_{256 * 1024};
    Shared::BufferRing ring_{};
    uint64_t descBaseIOVA_{0};
    uint64_t bufBaseIOVA_{0};

    void SetUp() override {
        auto descRegion = dma_.AllocateRegion(kNumBuffers * sizeof(Async::HW::OHCIDescriptor));
        ASSERT_TRUE(descRegion.has_value());
        descBaseIOVA_ = descRegion->deviceBase;

        auto bufRegion = dma_.AllocateRegion(kNumBuffers * kBufferSize);
        ASSERT_TRUE(bufRegion.has_value());
        bufBaseIOVA_ = bufRegion->deviceBase;

        auto* descs = reinterpret_cast<Async::HW::OHCIDescriptor*>(descRegion->virtualBase);
        std::span<Async::HW::OHCIDescriptor> descSpan{descs, kNumBuffers};
        std::span<uint8_t> bufSpan{bufRegion->virtualBase, kNumBuffers * kBufferSize};

        ASSERT_TRUE(ring_.Initialize(descSpan, bufSpan, kNumBuffers, kBufferSize));
        ring_.BindDma(&dma_);
        ASSERT_TRUE(ring_.Finalize(descBaseIOVA_, bufBaseIOVA_));
    }
};

TEST_F(BufferRingDMATest, FinalizeProgramsDataAddressAndBranchWords) {
    constexpr size_t kNum = 32;
    constexpr size_t kBufSize = 256;

    for (size_t i = 0; i < kNum; ++i) {
        auto* desc = ring_.GetDescriptor(i);
        ASSERT_NE(desc, nullptr);

        const uint32_t expectedData =
            static_cast<uint32_t>((bufBaseIOVA_ + i * kBufSize) & 0xFFFFFFFFu);
        EXPECT_EQ(desc->dataAddress, expectedData);

        const size_t nextIndex = (i + 1) % kNum;
        const uint32_t expectedNextDescAddr =
            static_cast<uint32_t>((descBaseIOVA_ + nextIndex * sizeof(Async::HW::OHCIDescriptor)) & 0xFFFFFFF0u);
        const uint32_t branchAddr = Async::HW::DecodeBranchPhys32_AR(desc->branchWord);
        EXPECT_EQ(branchAddr, expectedNextDescAddr);
        EXPECT_NE(desc->branchWord, 0u);
    }
}

TEST(DMASafeCopyTest, PreservesBytesFromFourByteAlignedEightByteMisalignedSource) {
    // Regression coverage for PR #3:
    // ARM64 can fault when downstream code touches DMA-backed AR payloads that are only
    // quadlet-aligned (4-byte aligned, but 8-byte misaligned). The shared helper must
    // preserve bytes correctly for that shape without relying on wider aligned loads.
    alignas(16) std::array<uint8_t, 128> source{};
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<uint8_t>(0x40u + i);
    }

    const uint8_t* misalignedSource = source.data() + 4;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(misalignedSource) % 4u, 0u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(misalignedSource) % 8u, 4u);

    constexpr std::array<size_t, 11> kLengths{1u, 2u, 3u, 4u, 5u, 8u, 12u, 16u, 20u, 31u, 32u};
    for (size_t length : kLengths) {
        std::array<uint8_t, 64> destination{};
        Common::CopyFromQuadletAlignedDeviceMemory(
            std::span<uint8_t>(destination.data(), length),
            misalignedSource);
        EXPECT_EQ(std::memcmp(destination.data(), misalignedSource, length), 0)
            << "length=" << length;
    }
}

TEST(DMASafeCopyTest, PreservesBytesAtObserved4148TailOffset) {
    // Regression coverage for PR #3 and the follow-up AR boundary crash:
    // the observed faulting source address was at offset 4148 inside a 4160-byte AR buffer,
    // which is 4-byte aligned but 8-byte misaligned.
    alignas(16) std::array<uint8_t, 4160 + 64> source{};
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = static_cast<uint8_t>(i & 0xFFu);
    }

    const uint8_t* crashShapeSource = source.data() + 4148;
    ASSERT_EQ(reinterpret_cast<uintptr_t>(crashShapeSource) % 4u, 0u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(crashShapeSource) % 8u, 4u);

    constexpr std::array<size_t, 3> kLengths{4u, 8u, 12u};
    for (size_t length : kLengths) {
        std::array<uint8_t, 16> destination{};
        Common::CopyFromQuadletAlignedDeviceMemory(
            std::span<uint8_t>(destination.data(), length),
            crashShapeSource);
        EXPECT_EQ(std::memcmp(destination.data(), crashShapeSource, length), 0)
            << "length=" << length;
    }
}

TEST_F(BufferRingDMATest, DequeuePrefersUnreadCurrentBufferBeforeAdvancingToNext) {
    auto* current = ring_.GetDescriptor(0);
    auto* next = ring_.GetDescriptor(1);
    ASSERT_NE(current, nullptr);
    ASSERT_NE(next, nullptr);

    constexpr uint16_t kReqCount = 256;

    // First interrupt: hardware filled 16 bytes in buffer 0.
    ASFW::Async::HW::AR_init_status(*current, static_cast<uint16_t>(kReqCount - 16));
    auto first = ring_.Dequeue();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->descriptorIndex, 0u);
    EXPECT_EQ(first->startOffset, 0u);
    EXPECT_EQ(first->bytesFilled, 16u);
    EXPECT_EQ(ring_.CommitConsumed(first->descriptorIndex, first->bytesFilled), kIOReturnSuccess);

    // Before software recycles buffer 0, hardware appends 16 more bytes there and also
    // starts writing into buffer 1. We must not skip the unread tail in buffer 0.
    ASFW::Async::HW::AR_init_status(*current, static_cast<uint16_t>(kReqCount - 32));
    ASFW::Async::HW::AR_init_status(*next, static_cast<uint16_t>(kReqCount - 16));

    auto second = ring_.Dequeue();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->descriptorIndex, 0u);
    EXPECT_EQ(second->startOffset, 16u);
    EXPECT_EQ(second->bytesFilled, 32u);
}

TEST_F(BufferRingDMATest, CommitConsumedWaitsForNewBytesBeforeReEmittingTail) {
    auto* current = ring_.GetDescriptor(0);
    ASSERT_NE(current, nullptr);

    constexpr uint16_t kReqCount = 256;

    ASFW::Async::HW::AR_init_status(*current, static_cast<uint16_t>(kReqCount - 32));

    auto first = ring_.Dequeue();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->descriptorIndex, 0u);
    EXPECT_EQ(first->startOffset, 0u);
    EXPECT_EQ(first->bytesFilled, 32u);

    // Simulate RxPath parsing only the first half of the buffer because the tail
    // contains an incomplete packet.
    EXPECT_EQ(ring_.CommitConsumed(first->descriptorIndex, 16u), kIOReturnSuccess);

    // With no new DMA writes, the ring should preserve the unread tail without
    // re-emitting the exact same bytes again in a tight loop.
    auto second = ring_.Dequeue();
    EXPECT_FALSE(second.has_value());

    // Once hardware appends more bytes to the same descriptor, the preserved
    // tail becomes visible again together with the new data.
    ASFW::Async::HW::AR_init_status(*current, static_cast<uint16_t>(kReqCount - 48));

    auto third = ring_.Dequeue();
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third->descriptorIndex, 0u);
    EXPECT_EQ(third->startOffset, 16u);
    EXPECT_EQ(third->bytesFilled, 48u);
}

TEST_F(BufferRingDMATest, CopyReadableBytesConcatenatesPreservedTailAndNextBuffer) {
    auto* current = ring_.GetDescriptor(0);
    auto* next = ring_.GetDescriptor(1);
    auto* currentBuffer = ring_.GetBufferAddress(0);
    auto* nextBuffer = ring_.GetBufferAddress(1);
    ASSERT_NE(current, nullptr);
    ASSERT_NE(next, nullptr);
    ASSERT_NE(currentBuffer, nullptr);
    ASSERT_NE(nextBuffer, nullptr);

    constexpr uint16_t kReqCount = 256;
    constexpr size_t kCurrentFilled = 32;
    constexpr size_t kNextFilled = 16;
    constexpr size_t kConsumedInCurrent = 20;

    for (size_t i = 0; i < kCurrentFilled; ++i) {
        currentBuffer[i] = static_cast<uint8_t>(i);
    }
    for (size_t i = 0; i < kNextFilled; ++i) {
        nextBuffer[i] = static_cast<uint8_t>(0x80u + i);
    }

    ASFW::Async::HW::AR_init_status(*current, static_cast<uint16_t>(kReqCount - kCurrentFilled));
    ASFW::Async::HW::AR_init_status(*next, static_cast<uint16_t>(kReqCount - kNextFilled));

    auto first = ring_.Dequeue();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(ring_.CommitConsumed(first->descriptorIndex, kConsumedInCurrent), kIOReturnSuccess);

    auto second = ring_.Dequeue();
    EXPECT_FALSE(second.has_value()) << "Unread tail in head buffer currently hides next-buffer data";

    std::array<uint8_t, 64> stitched{};
    const size_t copied = ring_.CopyReadableBytes(stitched);
    ASSERT_EQ(copied, (kCurrentFilled - kConsumedInCurrent) + kNextFilled);

    for (size_t i = 0; i < (kCurrentFilled - kConsumedInCurrent); ++i) {
        EXPECT_EQ(stitched[i], static_cast<uint8_t>(kConsumedInCurrent + i));
    }
    for (size_t i = 0; i < kNextFilled; ++i) {
        EXPECT_EQ(stitched[(kCurrentFilled - kConsumedInCurrent) + i], static_cast<uint8_t>(0x80u + i));
    }
}

TEST_F(BufferRingDMATest, ConsumeReadableBytesAcrossBoundaryLeavesPartialNextBufferVisible) {
    auto* current = ring_.GetDescriptor(0);
    auto* next = ring_.GetDescriptor(1);
    auto* currentBuffer = ring_.GetBufferAddress(0);
    auto* nextBuffer = ring_.GetBufferAddress(1);
    ASSERT_NE(current, nullptr);
    ASSERT_NE(next, nullptr);
    ASSERT_NE(currentBuffer, nullptr);
    ASSERT_NE(nextBuffer, nullptr);

    constexpr uint16_t kReqCount = 256;
    constexpr size_t kCurrentFilled = 32;
    constexpr size_t kNextFilled = 16;
    constexpr size_t kConsumedInCurrent = 20;
    constexpr size_t kCrossBoundaryConsume = 16; // 12 bytes tail + 4 bytes in next buffer

    std::memset(currentBuffer, 0x11, kCurrentFilled);
    std::memset(nextBuffer, 0x22, kNextFilled);

    ASFW::Async::HW::AR_init_status(*current, static_cast<uint16_t>(kReqCount - kCurrentFilled));
    ASFW::Async::HW::AR_init_status(*next, static_cast<uint16_t>(kReqCount - kNextFilled));

    auto first = ring_.Dequeue();
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(ring_.CommitConsumed(first->descriptorIndex, kConsumedInCurrent), kIOReturnSuccess);

    ASSERT_EQ(ring_.ConsumeReadableBytes(kCrossBoundaryConsume), kIOReturnSuccess);
    EXPECT_EQ(ring_.Head(), 1u);

    auto visible = ring_.Dequeue();
    ASSERT_TRUE(visible.has_value());
    EXPECT_EQ(visible->descriptorIndex, 1u);
    EXPECT_EQ(visible->startOffset, 4u);
    EXPECT_EQ(visible->bytesFilled, kNextFilled);
}

TEST_F(BufferRingDMATest, CopyReadableBytesReassemblesSplitReadQuadletResponse) {
    auto* current = ring_.GetDescriptor(0);
    auto* next = ring_.GetDescriptor(1);
    auto* currentBuffer = ring_.GetBufferAddress(0);
    auto* nextBuffer = ring_.GetBufferAddress(1);
    ASSERT_NE(current, nullptr);
    ASSERT_NE(next, nullptr);
    ASSERT_NE(currentBuffer, nullptr);
    ASSERT_NE(nextBuffer, nullptr);

    constexpr uint16_t kReqCount = 256;
    constexpr size_t kPacketSplitOffset = 244;
    constexpr size_t kCurrentFilled = 256;
    constexpr size_t kNextFilled = 8;

    const auto packet = MakeARDmaBufferFromHostWords({
        0xFFC1F160u,
        0xFFC00000u,
        0x00000000u,
        0x00000046u,
    });
    ASSERT_EQ(packet.size(), 20u);

    std::memset(currentBuffer, 0, kCurrentFilled);
    std::memset(nextBuffer, 0, kNextFilled);
    std::memcpy(currentBuffer + kPacketSplitOffset, packet.data(), 12);
    std::memcpy(nextBuffer, packet.data() + 12, 8);

    ASFW::Async::HW::AR_init_status(*current, static_cast<uint16_t>(kReqCount - kCurrentFilled));
    ASFW::Async::HW::AR_init_status(*next, static_cast<uint16_t>(kReqCount - kNextFilled));

    auto first = ring_.Dequeue();
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(ring_.CommitConsumed(first->descriptorIndex, kPacketSplitOffset), kIOReturnSuccess);

    auto truncated = ASFW::Async::ARPacketParser::ParseNext(
        std::span<const uint8_t>(currentBuffer, kCurrentFilled), kPacketSplitOffset);
    EXPECT_FALSE(truncated.has_value());

    std::array<uint8_t, ASFW::Async::ARPacketParser::kMaxPacketBytes> stitched{};
    const size_t copied = ring_.CopyReadableBytes(stitched);
    ASSERT_EQ(copied, packet.size());

    const auto parsed = ASFW::Async::ARPacketParser::ParseNext(
        std::span<const uint8_t>(stitched.data(), copied), 0);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->tCode, 0x6u);
    EXPECT_EQ(parsed->rCode, 0x0u);
    EXPECT_EQ(parsed->headerLength, 16u);
    EXPECT_EQ(parsed->totalLength, packet.size());
}

TEST_F(LargeBufferRingDMATest, CopyReadableBytesReassemblesSplitReadQuadletResponseAt4148Boundary) {
    // Regression for the live hardware failure that surfaced after PR #3:
    // once the RX path started preserving tails across AR buffers, the first stitched copy
    // happened at the same 4148/4160 alignment seam. Keep that exact boundary in tests.
    auto* current = ring_.GetDescriptor(0);
    auto* next = ring_.GetDescriptor(1);
    auto* currentBuffer = ring_.GetBufferAddress(0);
    auto* nextBuffer = ring_.GetBufferAddress(1);
    ASSERT_NE(current, nullptr);
    ASSERT_NE(next, nullptr);
    ASSERT_NE(currentBuffer, nullptr);
    ASSERT_NE(nextBuffer, nullptr);

    constexpr uint16_t kReqCount = static_cast<uint16_t>(kBufferSize);
    constexpr size_t kPacketSplitOffset = 4148;
    constexpr size_t kCurrentFilled = kBufferSize;
    constexpr size_t kNextFilled = 8;

    ASSERT_EQ(reinterpret_cast<uintptr_t>(currentBuffer + kPacketSplitOffset) % 4u, 0u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(currentBuffer + kPacketSplitOffset) % 8u, 4u);

    const auto packet = MakeARDmaBufferFromHostWords({
        0xFFC1F160u,
        0xFFC00000u,
        0x00000000u,
        0x00000046u,
    });
    ASSERT_EQ(packet.size(), 20u);

    std::memset(currentBuffer, 0, kCurrentFilled);
    std::memset(nextBuffer, 0, kNextFilled);
    std::memcpy(currentBuffer + kPacketSplitOffset, packet.data(), 12);
    std::memcpy(nextBuffer, packet.data() + 12, 8);

    ASFW::Async::HW::AR_init_status(*current, static_cast<uint16_t>(kReqCount - kCurrentFilled));
    ASFW::Async::HW::AR_init_status(*next, static_cast<uint16_t>(kReqCount - kNextFilled));

    auto first = ring_.Dequeue();
    ASSERT_TRUE(first.has_value());
    ASSERT_EQ(ring_.CommitConsumed(first->descriptorIndex, kPacketSplitOffset), kIOReturnSuccess);

    auto truncated = ASFW::Async::ARPacketParser::ParseNext(
        std::span<const uint8_t>(currentBuffer, kCurrentFilled), kPacketSplitOffset);
    EXPECT_FALSE(truncated.has_value());

    alignas(4) std::array<uint8_t, ASFW::Async::ARPacketParser::kMaxPacketBytes> stitched{};
    const size_t copied = ring_.CopyReadableBytes(stitched);
    ASSERT_EQ(copied, packet.size());

    const auto parsed = ASFW::Async::ARPacketParser::ParseNext(
        std::span<const uint8_t>(stitched.data(), copied), 0);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->tCode, 0x6u);
    EXPECT_EQ(parsed->rCode, 0x0u);
    EXPECT_EQ(parsed->headerLength, 16u);
    EXPECT_EQ(parsed->totalLength, packet.size());
}

} // namespace ASFW::Testing
