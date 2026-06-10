#include "TestHarness.hpp"

#include "../Core/VirtualAudioDeviceController.hpp"
#include "../Lab/VerifyingSlotProvider.hpp"
#include "../Protocols/Audio/DICE/DiceTxStreamEngine.hpp"

// Step 6 scenario pump (README, Milestone 1 exit criteria): WriteEnd-shaped
// schedules driving controller → engine → Verifying(Fake). The regular
// schedule must run >= 1e6 cycles with zero invariant violations; the
// adversarial schedules (irregular frame counts, skipped callback,
// sample-time jumps) must produce their *expected* counter signatures — a
// missed window shows up in the payload-writer miss buckets, never as a
// structural violation or silent corruption.

namespace ASFW::LabTests {

using Lab::VerifierCounterId;
using Lab::VerifierSnapshot;
using Lab::VerifyingSlotProvider;
using Protocols::Audio::AMDTP::HostAudioBufferView;
using Protocols::Audio::DICE::DiceDeviceIdentity;

namespace {

constexpr uint32_t kChannels = 2;
constexpr uint32_t kRingFrames = 4096;

// Matches DiceTxEngineTests: resolves to the Focusrite profile
// (Raw24-in-32 BE host->device encoding).
constexpr DiceDeviceIdentity kFocusriteIdentity{0x130E01020304ULL, 0x00130E, 1};

// The pump: owns the controller, the Verifying(Fake) interposition, and the
// host-side PCM ring (mirroring the WriteEnd producer: the view's pointer
// sits at ring offset firstFrame % frameCapacity; the payload writer owns
// the wrap arithmetic).
struct LabPump final {
    Driver::VirtualAudioDeviceController controller{};
    VerifyingSlotProvider verifier;
    float ring[kRingFrames * kChannels]{};
    uint32_t nextPacketIndex{0};
    uint64_t exposedFrames{0};
    uint64_t prepareFailures{0};

    LabPump() noexcept : verifier(controller.FakeSlotProvider()) {}

    bool Init() noexcept {
        if (!controller.Initialize() ||
            !controller.SelectProfile(kFocusriteIdentity) ||
            !controller.ConfigureOutputStream(48000, kChannels, kRingFrames)) {
            return false;
        }
        controller.BindLabSlotProvider(&verifier);
        controller.ResetTransportLab(0, 0);
        verifier.Reset();
        return true;
    }

    // Prepare packets until the exposed frame timeline covers endFrame.
    void PrepareCoverage(uint64_t endFrame) noexcept {
        while (exposedFrames < endFrame) {
            if (!controller.PrepareLabPacket(nextPacketIndex, 0xFFFF, false)) {
                ++prepareFailures;
                return;
            }
            const auto* published =
                controller.FakeSlotProvider().PublishedPacket(nextPacketIndex);
            if (published != nullptr && published->isData) {
                exposedFrames += published->framesInPacket;
            }
            ++nextPacketIndex;
        }
    }

    void FillConstant(uint64_t firstFrame, uint32_t frameCount,
                      float value) noexcept {
        for (uint32_t i = 0; i < frameCount; ++i) {
            const uint32_t pos =
                static_cast<uint32_t>((firstFrame + i) % kRingFrames);
            for (uint32_t ch = 0; ch < kChannels; ++ch) {
                ring[pos * kChannels + ch] = value;
            }
        }
    }

    HostAudioBufferView View(uint64_t firstFrame, uint32_t frameCount) noexcept {
        HostAudioBufferView view{};
        view.interleavedFloat32 =
            ring + (firstFrame % kRingFrames) * kChannels;
        view.firstFrame = firstFrame;
        view.frameCount = frameCount;
        view.frameCapacity = kRingFrames;
        view.channels = kChannels;
        return view;
    }

    // One well-behaved WriteEnd callback: cover, fill, submit.
    void Callback(uint64_t sampleTime, uint32_t frames, float value) noexcept {
        PrepareCoverage(sampleTime + frames);
        FillConstant(sampleTime, frames, value);
        controller.SubmitWriteEnd(View(sampleTime, frames));
    }
};

void CheckAllGreen(TestContext& ctx, const LabPump& pump) {
    const VerifierSnapshot snapshot = pump.verifier.Snapshot();
    CHECK_EQ_U64(ctx, snapshot.TotalViolations(), 0);
    CHECK(ctx, !snapshot.firstViolationValid);
    CHECK_EQ_U64(ctx, pump.prepareFailures, 0);
}

} // namespace

void RunVerifierScenarioTests(TestContext& ctx) {
    // Scenario A — regular schedule soak: >= 1e6 cycles, zero violations.
    {
        LabPump pump{};
        CHECK(ctx, pump.Init());

        uint64_t sampleTime = 0;
        while (pump.verifier.Snapshot().Value(
                   VerifierCounterId::kPacketsPublished) < 1000000) {
            pump.Callback(sampleTime, 512, 0.25f);
            sampleTime += 512;
        }

        CheckAllGreen(ctx, pump);
        const VerifierSnapshot snapshot = pump.verifier.Snapshot();
        const uint64_t total =
            snapshot.Value(VerifierCounterId::kPacketsPublished);
        const uint64_t data = snapshot.Value(VerifierCounterId::kDataPackets);
        CHECK(ctx, total >= 1000000);
        // Blocking 48k: 3 data per 4 packets. The tumbling-window check
        // enforces it per window; for the whole run a trailing partial
        // N,D,D,D group leaves 4*data - 3*total in [-3, 0].
        CHECK(ctx, data * 4 <= total * 3 && total * 3 - data * 4 <= 3);

        const auto& payload = pump.controller.PayloadCounters();
        CHECK_EQ_U64(ctx, payload.framesVisited.load(),
                     payload.framesWritten.load());
        CHECK_EQ_U64(ctx, payload.framesWithoutPacket.load(), 0);
        CHECK_EQ_U64(ctx, payload.framesOutsidePacket.load(), 0);
    }

    // Scenario B — irregular frame counts, contiguous sample time.
    {
        LabPump pump{};
        CHECK(ctx, pump.Init());

        constexpr uint32_t kSizes[] = {512, 480, 53, 8, 511, 997, 64, 256};
        uint64_t sampleTime = 0;
        for (int i = 0; i < 4000; ++i) {
            const uint32_t frames = kSizes[i % 8];
            pump.Callback(sampleTime, frames, 0.25f);
            sampleTime += frames;
        }

        CheckAllGreen(ctx, pump);
        const auto& payload = pump.controller.PayloadCounters();
        CHECK_EQ_U64(ctx, payload.framesVisited.load(),
                     payload.framesWritten.load());
        CHECK_EQ_U64(ctx, payload.framesWithoutPacket.load(), 0);
        CHECK_EQ_U64(ctx, payload.framesOutsidePacket.load(), 0);
    }

    // Scenario C — skipped callback: the gap frames are never written and
    // must surface as silence in structurally valid packets, not as a
    // violation. (Skip late in the run so the gap packets are still live in
    // the 256-slot fake ring for payload inspection.)
    {
        LabPump pump{};
        CHECK(ctx, pump.Init());

        uint64_t sampleTime = 0;
        for (int i = 0; i < 22; ++i) {
            pump.Callback(sampleTime, 512, 0.5f);
            sampleTime += 512;
        }

        const uint64_t gapStart = sampleTime;
        sampleTime += 512; // the HAL skipped this callback entirely
        const uint64_t gapEnd = sampleTime;

        for (int i = 0; i < 2; ++i) {
            pump.Callback(sampleTime, 512, 0.5f);
            sampleTime += 512;
        }

        CheckAllGreen(ctx, pump);
        const auto& payload = pump.controller.PayloadCounters();
        CHECK_EQ_U64(ctx, payload.framesVisited.load(),
                     payload.framesWritten.load());
        CHECK_EQ_U64(ctx, payload.framesVisited.load(), sampleTime - 512);

        // Find a data packet inside the gap and one in the written region.
        bool sawSilentGapPacket = false;
        bool sawWrittenPacket = false;
        const uint32_t scanStart =
            (pump.nextPacketIndex > 256) ? pump.nextPacketIndex - 256 : 0;
        for (uint32_t index = scanStart; index < pump.nextPacketIndex; ++index) {
            const auto* packet =
                pump.controller.FakeSlotProvider().PublishedPacket(index);
            if (packet == nullptr || packet->packetIndex != index ||
                !packet->isData) {
                continue;
            }
            const uint8_t* payloadBytes =
                pump.controller.FakeSlotProvider().SlotBytes(index) + 8;
            const uint32_t payloadSize =
                packet->framesInPacket * packet->dbs * 4;

            bool allZero = true;
            for (uint32_t i = 0; i < payloadSize; ++i) {
                if (payloadBytes[i] != 0) {
                    allZero = false;
                    break;
                }
            }

            if (packet->firstAudioFrame >= gapStart &&
                packet->firstAudioFrame + packet->framesInPacket <= gapEnd) {
                sawSilentGapPacket = sawSilentGapPacket || allZero;
                CHECK(ctx, allZero);
            } else if (packet->firstAudioFrame >= gapEnd ||
                       packet->firstAudioFrame + packet->framesInPacket <=
                           gapStart) {
                // Written region (the 256-slot ring may retain only the
                // post-gap side; pre-gap packets are typically evicted).
                sawWrittenPacket = sawWrittenPacket || !allZero;
            }
        }
        CHECK(ctx, sawSilentGapPacket);
        CHECK(ctx, sawWrittenPacket);
    }

    // Scenario D/E — sample-time jumps: ahead of the exposed timeline lands
    // in framesWithoutPacket; behind the retired/evicted window lands in
    // framesOutsidePacket. Structure stays green throughout.
    {
        LabPump pump{};
        CHECK(ctx, pump.Init());

        uint64_t sampleTime = 0;
        for (int i = 0; i < 8; ++i) { // warmup wraps the 256-slot ring
            pump.Callback(sampleTime, 512, 0.25f);
            sampleTime += 512;
        }

        const auto& payload = pump.controller.PayloadCounters();
        const uint64_t writtenBefore = payload.framesWritten.load();

        // D: a window far ahead of anything exposed (no PrepareCoverage).
        const uint64_t jumpedTime = sampleTime + 1000000;
        pump.FillConstant(jumpedTime, 512, 0.25f);
        pump.controller.SubmitWriteEnd(pump.View(jumpedTime, 512));
        CHECK_EQ_U64(ctx, payload.framesWithoutPacket.load(), 512);
        CHECK_EQ_U64(ctx, payload.framesWritten.load(), writtenBefore);

        // E: a window behind the live ring (frame 0 is long evicted).
        pump.FillConstant(0, 512, 0.25f);
        pump.controller.SubmitWriteEnd(pump.View(0, 512));
        CHECK_EQ_U64(ctx, payload.framesOutsidePacket.load(), 512);
        CHECK_EQ_U64(ctx, payload.framesWritten.load(), writtenBefore);

        CheckAllGreen(ctx, pump);

        // Normal pacing resumes cleanly after both faults.
        for (int i = 0; i < 4; ++i) {
            pump.Callback(sampleTime, 512, 0.25f);
            sampleTime += 512;
        }
        CheckAllGreen(ctx, pump);
        CHECK_EQ_U64(ctx, payload.framesWithoutPacket.load(), 512);
        CHECK_EQ_U64(ctx, payload.framesOutsidePacket.load(), 512);
    }

    // Scenario F — stream restart: engine reset + verifier reset, then green.
    {
        LabPump pump{};
        CHECK(ctx, pump.Init());

        uint64_t sampleTime = 0;
        for (int i = 0; i < 8; ++i) {
            pump.Callback(sampleTime, 512, 0.25f);
            sampleTime += 512;
        }
        CheckAllGreen(ctx, pump);

        pump.controller.ResetTransportLab(0, 0);
        pump.verifier.Reset();
        pump.exposedFrames = 0;

        sampleTime = 0;
        for (int i = 0; i < 8; ++i) {
            pump.Callback(sampleTime, 512, 0.25f);
            sampleTime += 512;
        }
        CheckAllGreen(ctx, pump);
    }
}

} // namespace ASFW::LabTests
