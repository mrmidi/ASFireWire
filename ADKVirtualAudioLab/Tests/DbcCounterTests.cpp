#include "TestHarness.hpp"

#include "../Protocols/Audio/IEC61883/DbcCounter.hpp"

namespace ASFW::LabTests {

using Protocols::Audio::IEC61883::DbcCounter;

void RunDbcCounterTests(TestContext& ctx) {
    DbcCounter dbc;

    dbc.Reset(0);
    CHECK_EQ_U32(ctx, dbc.ValueForNextPacket(), 0);

    // Advance-after-emit: 8 data blocks per blocking 48k data packet
    dbc.AdvanceDataBlocks(8);
    CHECK_EQ_U32(ctx, dbc.ValueForNextPacket(), 8);

    // No-data packet: advance by zero leaves the counter unchanged
    dbc.AdvanceDataBlocks(0);
    CHECK_EQ_U32(ctx, dbc.ValueForNextPacket(), 8);

    // Wrap at 256
    dbc.Reset(0xF8);
    dbc.AdvanceDataBlocks(8);
    CHECK_EQ_U32(ctx, dbc.ValueForNextPacket(), 0x00);

    dbc.Reset(0xFF);
    dbc.AdvanceDataBlocks(8);
    CHECK_EQ_U32(ctx, dbc.ValueForNextPacket(), 0x07);

    // Long-run continuity: 10^6 data packets of 8 blocks ≡ (10^6 * 8) mod 256
    dbc.Reset(0);
    for (int i = 0; i < 1000000; ++i) {
        dbc.AdvanceDataBlocks(8);
    }
    CHECK_EQ_U32(ctx, dbc.ValueForNextPacket(), (1000000ull * 8) % 256);
}

} // namespace ASFW::LabTests
