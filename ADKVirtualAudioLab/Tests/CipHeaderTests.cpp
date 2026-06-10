#include "TestHarness.hpp"

#include "../Protocols/Audio/IEC61883/CipHeader.hpp"

namespace ASFW::LabTests {

using namespace Protocols::Audio::IEC61883;

void RunCipHeaderTests(TestContext& ctx) {
    // Canonical 48 kHz stereo AM824 stream: sid=0, dbs=2, fmt=0x10, fdf=0x02
    CipHeaderConfig config{};
    config.sid = 0;
    config.dbs = 2;
    config.fmt = 0x10;
    config.fdf = 0x02;
    CipHeaderBuilder builder(config);

    // Data packet, dbc=0x10, syt=0x1234:
    //   q0 = dbs<<16 | dbc           = 0x00020010
    //   q1 = EOH | fmt<<24 | fdf<<16 | syt = 0x90021234
    {
        const CipHeaderWords words = builder.BuildData(0x10, 0x1234);
        CHECK_EQ_U32(ctx, words.q0, 0x00020010);
        CHECK_EQ_U32(ctx, words.q1, 0x90021234);
    }

    // No-data packet (Linux amdtp-stream.c + FFADO verified):
    //   FDF=0xFF, SYT=0xFFFF, DBS unchanged, DBC carried unchanged.
    {
        const CipHeaderWords words = builder.BuildNoData(0x10);
        CHECK_EQ_U32(ctx, words.q0, 0x00020010);
        CHECK_EQ_U32(ctx, words.q1, 0x90FFFFFF);
    }

    // Non-zero SID lands in q0[29:24] and is masked to 6 bits
    {
        CipHeaderConfig c2 = config;
        c2.sid = 0x3F;
        CipHeaderBuilder b2(c2);
        CHECK_EQ_U32(ctx, b2.BuildData(0, 0xFFFF).q0, 0x3F020000);

        c2.sid = 0xFF; // out-of-range bits must not leak into [31:30]
        CipHeaderBuilder b3(c2);
        CHECK_EQ_U32(ctx, b3.BuildData(0, 0xFFFF).q0, 0x3F020000);
    }

    // SPH flag is bit 10 of q0
    {
        CipHeaderConfig c2 = config;
        c2.sph = true;
        CipHeaderBuilder b2(c2);
        CHECK_EQ_U32(ctx, b2.BuildData(0, 0).q0, 0x00020400);
    }

    // DBC occupies exactly [7:0]
    {
        const CipHeaderWords words = builder.BuildData(0xFF, 0);
        CHECK_EQ_U32(ctx, words.q0 & 0xFFu, 0xFF);
        CHECK_EQ_U32(ctx, words.q0 & ~0x000200FFu, 0);
    }
}

} // namespace ASFW::LabTests
