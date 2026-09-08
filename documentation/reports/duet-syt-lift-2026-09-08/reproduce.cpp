// Standalone diagnostic: exercises ASFW's current headers, without hardware.
// This characterizes the defect; these assertions are not desired behavior.
#include "Audio/Runtime/HardwareSampleTimeline.hpp"
#include "Audio/Wire/AMDTP/RxSequenceReplay.hpp"

#include <cassert>
#include <cstdio>

using namespace ASFW::Audio::Runtime;

int main() {
    ASFW::Timing::gHostTimebaseInfo = {125, 3};
    constexpr uint32_t receiveCycle = 0x8ca9f000;
    constexpr uint16_t syt = 0x1159;
    constexpr uint32_t delay = 12800;
    constexpr uint64_t frame = 12288;
    constexpr uint64_t drainHost = 8897650550747;
    constexpr uint64_t age = 20679;
    constexpr uint64_t loggedHost = 8897650584889;
    constexpr uint64_t sytPeriod = 16 * ASFW::Timing::kTicksPerCycle;

    const auto fields = ASFW::Timing::decodeCycleTimer(receiveCycle);
    const uint64_t packetBus = ASFW::Timing::tstampToOffsets(fields);
    // Direct forward lift of the received SYT's four-bit cycle coordinate.
    // Independent oracle: the receive lift in FFADO cycletimer.h:390-440
    // uses this receive-cycle reference, without a transfer-delay threshold.
    const uint32_t forwardCycles = ((syt >> 12) + 16 - (fields.cycle % 16)) % 16;
    const uint32_t directLead = forwardCycles * ASFW::Timing::kTicksPerCycle +
                                (syt & 0xfff);
    const uint32_t replay = ComputeReplaySytOffset(syt, receiveCycle, delay);
    const uint32_t reconstructedLead = replay + delay;
    assert(directLead == 6489);
    assert(replay == 42841);
    assert(reconstructedLead - directLead == sytPeriod);
    assert(ComputeReplaySyt(replay, receiveCycle, delay) == syt);
    // Replay-delay configuration can change the chosen absolute period even
    // though the received packet and reconstructed wire SYT do not change.
    const auto shorterDelayReplay = ComputeReplaySytOffset(syt, receiveCycle, 6000);
    assert(shorterDelayReplay + 6000 == directLead);
    assert(ComputeReplaySyt(shorterDelayReplay, receiveCycle, 6000) == syt);
    std::printf("directLead=%u replayOffset=%u reconstructedLead=%u extra=%llu FW ticks\n",
                directLead, replay, reconstructedLead,
                static_cast<unsigned long long>(sytPeriod));

    const auto hostDelta = [](uint64_t ticks) {
        return ASFW::Timing::nanosToHostTicks(
            ticks * 1'000'000'000ULL / ASFW::Timing::kTicksPerSecond);
    };
    const uint64_t packetHost = drainHost - hostDelta(age);
    const auto observe = [&](HardwareSampleTimeline& timeline, uint64_t lead) {
        const auto epoch = timeline.BeginEpoch(HardwareTimelineSource::Receive,
            HardwareTimelineDiscontinuity::StartIO, 48000, 0);
        HardwareZeroTimestamp out{};
        assert(timeline.Observe({
            .epoch = epoch, .source = HardwareTimelineSource::Receive,
            .sampleFrame = frame, .frameCount = 8,
            .presentationBusTicks = packetBus + lead,
            .correlationBusTicks = packetBus, .correlationHostTicks = packetHost,
        }, &out) == HardwareObservationResult::BoundaryReady);
        return out;
    };
    HardwareSampleTimeline current{}, direct{};
    const auto currentAnchor = observe(current, reconstructedLead);
    const auto directAnchor = observe(direct, directLead);
    assert(currentAnchor.hostTicks == loggedHost);
    assert(currentAnchor.hostTicks - directAnchor.hostTicks == 48000);
    std::printf("currentHost=%llu directHost=%llu difference=2000 us (48000 host ticks)\n",
                static_cast<unsigned long long>(currentAnchor.hostTicks),
                static_cast<unsigned long long>(directAnchor.hostTicks));

    // Both absolute RX and TX coordinates currently inherit the same lift.
    // Show why changing only RX moves the first TX content frame by 96.
    constexpr uint64_t planAhead = 100 * ASFW::Timing::kTicksPerCycle;
    TxPresentationRange bothOld{}, rxOnly{}, bothDirect{};
    assert(current.PreviewTxRange(current.Epoch(),
        packetBus + planAhead + reconstructedLead, 8, bothOld));
    assert(direct.PreviewTxRange(direct.Epoch(),
        packetBus + planAhead + reconstructedLead, 8, rxOnly));
    assert(direct.PreviewTxRange(direct.Epoch(),
        packetBus + planAhead + directLead, 8, bothDirect));
    assert(rxOnly.firstAudioFrame == bothOld.firstAudioFrame + 96);
    assert(bothDirect.firstAudioFrame == bothOld.firstAudioFrame);
    std::printf("TX first frame: current=%llu RX-only change=%llu both direct=%llu\n",
                static_cast<unsigned long long>(bothOld.firstAudioFrame),
                static_cast<unsigned long long>(rxOnly.firstAudioFrame),
                static_cast<unsigned long long>(bothDirect.firstAudioFrame));

    // The replay representation is continuous only modulo 16 cycles.
    // Crossing the configured delay threshold creates an absolute-time jump.
    const auto atSourceZero = ASFW::Timing::encodeCycleTimer(0, 0, 0);
    const auto encode = [](uint32_t lead) {
        return static_cast<uint16_t>(((lead / 3072) << 12) | (lead % 3072));
    };
    const auto before = ComputeReplaySytOffset(encode(delay - 1), atSourceZero, delay) + delay;
    const auto after = ComputeReplaySytOffset(encode(delay), atSourceZero, delay) + delay;
    assert(static_cast<int64_t>(after) - before == 1 - static_cast<int64_t>(sytPeriod));
    std::printf("threshold: direct 12799 -> 12800, reconstructed %u -> %u (step -49151 ticks)\n",
                before, after);
}
