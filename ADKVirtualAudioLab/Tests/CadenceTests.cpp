#include "TestHarness.hpp"

#include "../Protocols/Audio/AMDTP/AmdtpCadence.hpp"

namespace ASFW::LabTests {

using Protocols::Audio::AMDTP::Blocking48kCadence;
using Protocols::Audio::AMDTP::NonBlocking48kCadence;

void RunCadenceTests(TestContext& ctx) {
    // --- Blocking 48k: N,D,D,D pattern, 6000 data per 8000 cycles ---
    {
        Blocking48kCadence cadence;
        cadence.Reset();

        // First four cycles spell out the pattern explicitly
        CHECK(ctx, !cadence.CurrentCycleIsData()); // pending 0
        CHECK_EQ_U32(ctx, cadence.CurrentCycleDataFrames(), 0);
        cadence.AdvanceCycle();
        CHECK(ctx, cadence.CurrentCycleIsData()); // pending 6
        CHECK_EQ_U32(ctx, cadence.CurrentCycleDataFrames(), 8);
        cadence.AdvanceCycle();
        CHECK(ctx, cadence.CurrentCycleIsData()); // pending 4
        cadence.AdvanceCycle();
        CHECK(ctx, cadence.CurrentCycleIsData()); // pending 2
        cadence.AdvanceCycle();
        CHECK(ctx, !cadence.CurrentCycleIsData()); // pending 0 again

        // One second of bus time: exactly 6000 data packets in 8000 cycles,
        // total frames exactly 48000
        cadence.Reset();
        uint32_t dataPackets = 0;
        uint64_t frames = 0;
        for (uint32_t cycle = 0; cycle < 8000; ++cycle) {
            if (cadence.CurrentCycleIsData()) {
                ++dataPackets;
                frames += cadence.CurrentCycleDataFrames();
            }
            cadence.AdvanceCycle();
        }
        CHECK_EQ_U32(ctx, dataPackets, 6000);
        CHECK_EQ_U64(ctx, frames, 48000);
        CHECK_EQ_U64(ctx, cadence.TotalCycles(), 8000);

        // Long-run stability: ratio holds exactly over 10^6 cycles
        cadence.Reset();
        uint64_t longFrames = 0;
        for (uint32_t cycle = 0; cycle < 1000000; ++cycle) {
            longFrames += cadence.CurrentCycleDataFrames();
            cadence.AdvanceCycle();
        }
        CHECK_EQ_U64(ctx, longFrames, 6000000); // 10^6 cycles * 6 frames/cycle
    }

    // --- Non-blocking 48k: every cycle data, 6 frames each ---
    {
        NonBlocking48kCadence cadence;
        cadence.Reset();
        uint64_t frames = 0;
        for (uint32_t cycle = 0; cycle < 8000; ++cycle) {
            CHECK(ctx, cadence.CurrentCycleIsData());
            frames += cadence.CurrentCycleDataFrames();
            cadence.AdvanceCycle();
        }
        CHECK_EQ_U64(ctx, frames, 48000);
        CHECK_EQ_U64(ctx, cadence.TotalCycles(), 8000);
    }
}

} // namespace ASFW::LabTests
