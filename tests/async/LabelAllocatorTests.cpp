#include <gtest/gtest.h>
#include <array>

#include "ASFWDriver/Async/Track/LabelAllocator.hpp"

using ASFW::Async::LabelAllocator;

// Round-robin allocate/free should advance to the next slot.
TEST(LabelAllocator, AllocateFreeRotates) {
    LabelAllocator alloc;

    const uint8_t first = alloc.Allocate();
    ASSERT_NE(first, LabelAllocator::kInvalidLabel);

    alloc.Free(first);

    const uint8_t second = alloc.Allocate();
    EXPECT_EQ(static_cast<uint8_t>(first + 1), second);
}

// Allocating all 64 labels should exhaust the bitmap, then freeing one reopens a slot.
TEST(LabelAllocator, ExhaustAndRecover) {
    LabelAllocator alloc;
    std::array<uint8_t, 64> labels{};

    for (size_t i = 0; i < labels.size(); ++i) {
        labels[i] = alloc.Allocate();
        ASSERT_NE(labels[i], LabelAllocator::kInvalidLabel) << "failed at index " << i;
    }

    EXPECT_EQ(alloc.Allocate(), LabelAllocator::kInvalidLabel) << "allocator should report full";

    alloc.Free(labels[10]);  // free an arbitrary slot
    EXPECT_EQ(alloc.Allocate(), labels[10]) << "allocator should return the freed slot first";
}

// NextLabel() must wrap 63→0 and never return an out-of-range value.
TEST(LabelAllocator, NextLabelWraps) {
    LabelAllocator alloc;
    for (int i = 0; i < 70; ++i) {
        const uint8_t lbl = alloc.NextLabel();
        const uint8_t expected = static_cast<uint8_t>(i & 0x3F);
        EXPECT_EQ(lbl, expected) << "iteration " << i;
    }
}
