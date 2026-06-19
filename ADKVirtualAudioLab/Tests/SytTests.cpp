#include "TestHarness.hpp"

#include "../Protocols/Audio/IEC61883/Syt.hpp"

namespace ASFW::LabTests {

using Protocols::Audio::IEC61883::SytFormatter;

void RunSytTests(TestContext& ctx) {
    CHECK(ctx, SytFormatter::IsNoInfo(0xFFFF));
    CHECK(ctx, !SytFormatter::IsNoInfo(0x1234));

    CHECK_EQ_U32(ctx, SytFormatter::NoInfo().value, 0xFFFF);
    CHECK(ctx, !SytFormatter::NoInfo().valid);

    // [15:12] = cycle mod 16, [11:0] = offset ticks
    CHECK_EQ_U32(ctx, SytFormatter::EncodeCycleOffset(0x1A2, 0x345), 0x2345);
    CHECK_EQ_U32(ctx, SytFormatter::EncodeCycleOffset(0, 0), 0x0000);
    CHECK_EQ_U32(ctx, SytFormatter::EncodeCycleOffset(15, 3071), 0xFBFF);
    // Cycle wraps mod 16; offset masked to 12 bits
    CHECK_EQ_U32(ctx, SytFormatter::EncodeCycleOffset(16, 0x1000), 0x0000);
    CHECK_EQ_U32(ctx, SytFormatter::EncodeCycleOffset(7999, 0), 0xF000); // 7999 % 16 = 15
}

} // namespace ASFW::LabTests
