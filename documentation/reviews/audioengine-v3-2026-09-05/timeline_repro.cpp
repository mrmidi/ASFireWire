#include "Audio/Runtime/HardwareSampleTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpCadence.hpp"
#include "Audio/Shared/AudioGeometryPolicy.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace ASFW::Audio::Runtime;
using namespace ASFW::Protocols::Audio::AMDTP;

int main() {
    ASFW::Timing::gHostTimebaseInfo = {1, 1};
    for (unsigned group : {1u, 6u, 12u}) {
        HardwareSampleTimeline t;
        auto epoch = t.BeginEpoch(HardwareTimelineSource::Transmit,
            HardwareTimelineDiscontinuity::StartIO, 48000, 0);
        BlockingCadence cadence;
        uint64_t frame = 0;
        std::vector<uint64_t> boundaries;
        // Two seconds, actual production blocking cadence. Mirror the
        // ObserveTxHardware policy: inspect the latest packet of each wake.
        for (uint64_t packet = 0; packet < 16000; ++packet) {
            unsigned frames = cadence.CurrentCycleDataFrames();
            if (frames && (packet + 1) % group == 0) {
                HardwareZeroTimestamp z;
                auto r = t.Observe({epoch, HardwareTimelineSource::Transmit,
                    frame, frames, 1000000 + packet * 3072,
                    1000000 + packet * 3072, 1000000000 + packet * 125000}, &z);
                if (r == HardwareObservationResult::BoundaryReady)
                    boundaries.push_back(z.sampleFrame);
            }
            frame += frames;
            cadence.AdvanceCycle();
        }
        std::printf("TX latest-only wake=%u packets: anchors=%zu frames=", group, boundaries.size());
        for (auto b : boundaries) std::printf(" %llu", (unsigned long long)b);
        std::puts("");
    }
    HardwareSampleTimeline t;
    auto e = t.BeginEpoch(HardwareTimelineSource::Transmit,
        HardwareTimelineDiscontinuity::StartIO, 48000, 0);
    TxPresentationRange range;
    for (uint64_t frame = 0; frame < 9000; frame += 8) {
        assert(t.PreviewTxRange(e, 1000000 + frame * 512, 8, range));
        assert(t.CommitTxRange(range));
    }
    assert(t.Observe({e, HardwareTimelineSource::Transmit, 8192, 8,
        1000000,1000000,1000000000}) == HardwareObservationResult::BoundaryReady);
    auto oldNext = t.NextTxFrame();
    auto newBase = HardwareSampleTimeline::NextBoundaryAfter(t.LastPublishedBoundary());
    auto newEpoch = t.BeginEpoch(HardwareTimelineSource::Transmit,
        HardwareTimelineDiscontinuity::PresentationLoss, 48000, newBase);
    assert(t.PreviewTxRange(newEpoch, 1004096, 8, range));
    std::printf("epoch: next planned frame %llu -> %llu (gap %llu frames); RX indexing is untouched by caller\n",
        (unsigned long long)oldNext, (unsigned long long)range.firstAudioFrame,
        (unsigned long long)(range.firstAudioFrame-oldNext));
    const auto safety = ASFW::Audio::Shared::AudioGeometryPolicy::RequiredOutputSafetyFrames(50, 48000);
    std::printf("Duet policy: output safety=%u; presentation-relative freeze lead with recorded 53876 ticks = %.3f frames\n",
        safety, 8.0 * 6 + 53876.0 / 512);
}
