#include "TestHarness.hpp"

#include "../Lab/WriteEndTraceReplayer.hpp"

#include <cstring>

namespace ASFW::LabTests {

using Lab::TraceParseResult;
using Lab::WriteEndEvent;
using Lab::WriteEndTraceReplayer;

void RunWriteEndTraceReplayerTests(TestContext& ctx) {
    // Comments, blank lines, whitespace, and a trailing line without newline.
    {
        const char* trace =
            "# WriteEnd trace: sample_time host_time frame_count\n"
            "\n"
            "0 1000000 512\n"
            "  512   1256000   512   # inline comment\n"
            "\t1024\t1512000\t480\n"
            "1504 1768000 512";
        WriteEndEvent events[8]{};
        const TraceParseResult result = WriteEndTraceReplayer::ParseTraceText(
            trace, std::strlen(trace), events, 8);

        CHECK_EQ_U64(ctx, result.eventCount, 4);
        CHECK_EQ_U64(ctx, result.malformedLines, 0);
        CHECK(ctx, !result.truncated);
        CHECK_EQ_U64(ctx, events[0].sampleTime, 0);
        CHECK_EQ_U64(ctx, events[0].hostTime, 1000000);
        CHECK_EQ_U32(ctx, events[0].frameCount, 512);
        CHECK_EQ_U64(ctx, events[2].sampleTime, 1024);
        CHECK_EQ_U32(ctx, events[2].frameCount, 480);
        CHECK_EQ_U64(ctx, events[3].sampleTime, 1504);
    }

    // Malformed lines are counted and skipped, never silently absorbed.
    {
        const char* trace =
            "0 100 512\n"
            "not a line\n"
            "1 2\n"                       // missing field
            "512 200 512 junk\n"          // trailing junk
            "512 200 4294967296\n"        // frame count > uint32
            "99999999999999999999 1 1\n"  // u64 overflow
            "1024 300 512\n";
        WriteEndEvent events[8]{};
        const TraceParseResult result = WriteEndTraceReplayer::ParseTraceText(
            trace, std::strlen(trace), events, 8);

        CHECK_EQ_U64(ctx, result.eventCount, 2);
        CHECK_EQ_U64(ctx, result.malformedLines, 5);
        CHECK_EQ_U64(ctx, events[1].sampleTime, 1024);
    }

    // Capacity exhaustion reports truncation instead of dropping silently.
    {
        const char* trace = "0 1 512\n512 2 512\n1024 3 512\n";
        WriteEndEvent events[2]{};
        const TraceParseResult result = WriteEndTraceReplayer::ParseTraceText(
            trace, std::strlen(trace), events, 2);

        CHECK_EQ_U64(ctx, result.eventCount, 2);
        CHECK(ctx, result.truncated);
    }

    // Replay visits every event in order.
    {
        const char* trace = "0 1 512\n512 2 480\n992 3 512\n";
        WriteEndEvent events[4]{};
        const TraceParseResult result = WriteEndTraceReplayer::ParseTraceText(
            trace, std::strlen(trace), events, 4);
        CHECK_EQ_U64(ctx, result.eventCount, 3);

        uint64_t frames = 0;
        uint64_t lastSample = 0;
        WriteEndTraceReplayer::Replay(events, result.eventCount,
                                      [&](const WriteEndEvent& event) {
                                          frames += event.frameCount;
                                          lastSample = event.sampleTime;
                                      });
        CHECK_EQ_U64(ctx, frames, 1504);
        CHECK_EQ_U64(ctx, lastSample, 992);
    }
}

} // namespace ASFW::LabTests
