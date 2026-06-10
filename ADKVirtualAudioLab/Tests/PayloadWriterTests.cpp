#include "TestHarness.hpp"

#include "../Protocols/Audio/AMDTP/AmdtpPacketTimeline.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpPayloadWriter.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpTxPacketizer.hpp"
#include "../Protocols/Audio/AMDTP/PcmSlotCodec.hpp"

namespace ASFW::LabTests {

using namespace Protocols::Audio::AMDTP;

namespace {

AmdtpStreamConfig MakeConfig48kBlocking(uint8_t pcmChannels,
                                        uint8_t midiSlots) {
    AmdtpStreamConfig config{};
    config.sampleRate = 48000;
    config.streamMode = StreamMode::Blocking;
    config.sid = 0;
    config.dbs = 0; // derived
    config.pcmChannels = pcmChannels;
    config.midiSlots = midiSlots;
    config.fmt = 0x10;
    config.fdf = 0x02;
    config.framesPerDataPacket = 8;
    config.maxPacketBytes = 512;
    return config;
}

struct WriterFixture final {
    static constexpr uint32_t kSlots = 16;
    static constexpr uint32_t kCapacity = 512;

    AmdtpTxPacketizer packetizer{};
    AmdtpPacketTimeline timeline{};
    AmdtpPayloadWriter writer{};
    PacketTimelineSlot timelineSlots[kSlots]{};
    uint8_t bytes[kSlots][kCapacity]{};
    uint32_t nextPacketIndex{0};

    bool Setup(const AmdtpStreamConfig& config, const AmdtpTxPolicy& policy) {
        if (!timeline.AttachSlots(timelineSlots, kSlots)) {
            return false;
        }
        packetizer.BindTimeline(&timeline);
        if (!packetizer.Configure(config, policy)) {
            return false;
        }
        writer.Configure(packetizer.StreamConfig(), policy); // dbs derived
        writer.BindTimeline(&timeline);
        return true;
    }

    // Blocking 48k cadence is N,D,D,D — 4 cycles expose 24 frames.
    bool PrepareCycles(uint32_t count) {
        const AmdtpTimingState noClock{};
        PreparedTxPacket packet{};
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t idx = nextPacketIndex++;
            const TxPacketSlotView slot{idx, bytes[idx % kSlots], kCapacity};
            if (!packetizer.PrepareNextPacket(slot, noClock, packet)) {
                return false;
            }
        }
        return true;
    }
};

struct CounterSnapshot final {
    uint64_t visited{0};
    uint64_t written{0};
    uint64_t withoutPacket{0};
    uint64_t outsidePacket{0};
};

CounterSnapshot Snap(const AmdtpPayloadWriterCounters& counters) {
    return CounterSnapshot{counters.framesVisited.load(),
                           counters.framesWritten.load(),
                           counters.framesWithoutPacket.load(),
                           counters.framesOutsidePacket.load()};
}

bool Balanced(const CounterSnapshot& s) {
    return s.visited == s.written + s.withoutPacket + s.outsidePacket;
}

// Deterministic per-frame, per-channel sample inside [-1, +1].
float SampleFor(uint64_t frame, uint32_t channel) {
    const float base = static_cast<float>(frame % 32) / 32.0f;
    return (channel % 2 == 0) ? base : -base;
}

uint32_t ReadSlotQuadlet(const uint8_t* packetBytes, uint32_t frameInPacket,
                         uint32_t dbs, uint32_t slotIdx) {
    const uint8_t* p = packetBytes + 8 + (frameInPacket * dbs + slotIdx) * 4;
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

bool FrameMatches(const uint8_t* packetBytes, uint32_t frameInPacket,
                  uint32_t dbs, uint64_t absoluteFrame, uint32_t pcmChannels,
                  PcmSlotEncoding encoding) {
    for (uint32_t ch = 0; ch < pcmChannels; ++ch) {
        const uint32_t expected =
            PcmSlotCodec::EncodeFloat32(SampleFor(absoluteFrame, ch), encoding);
        if (ReadSlotQuadlet(packetBytes, frameInPacket, dbs, ch) != expected) {
            return false;
        }
    }
    return true;
}

void FillHostWindow(float* host, uint64_t firstFrame, uint32_t frameCount,
                    uint32_t channels) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            host[i * channels + ch] = SampleFor(firstFrame + i, ch);
        }
    }
}

} // namespace

void RunPayloadWriterTests(TestContext& ctx) {
    // --- Full window inside one packet: every frame written, bit-exact ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(2)); // N, D(frames 0..7)
        CHECK_EQ_U64(ctx, f.timeline.ExposedFrameEnd(), 8);

        float host[8 * 2];
        FillHostWindow(host, 0, 8, 2);
        const HostAudioBufferView view{host, 0, 8, 64, 2};
        f.writer.WriteFloat32Interleaved(view);

        const CounterSnapshot s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.visited, 8);
        CHECK_EQ_U64(ctx, s.written, 8);
        CHECK_EQ_U64(ctx, s.withoutPacket, 0);
        CHECK_EQ_U64(ctx, s.outsidePacket, 0);
        CHECK(ctx, Balanced(s));

        bool pcmOk = true;
        for (uint32_t i = 0; i < 8; ++i) {
            pcmOk = pcmOk && FrameMatches(f.bytes[1], i, 2, i, 2,
                                          PcmSlotEncoding::Am824MBLA);
        }
        CHECK(ctx, pcmOk);

        // The write must not touch the CIP header.
        CHECK_EQ_U32(ctx, f.bytes[1][0], 0x00); // sid
        CHECK_EQ_U32(ctx, f.bytes[1][1], 0x02); // dbs
        CHECK_EQ_U32(ctx, f.bytes[1][4], 0x90); // EOH|FMT
    }

    // --- Window spanning three packets ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(4)); // N, D(0..7), D(8..15), D(16..23)
        CHECK_EQ_U64(ctx, f.timeline.ExposedFrameEnd(), 24);

        float host[16 * 2];
        FillHostWindow(host, 4, 16, 2);
        const HostAudioBufferView view{host, 4, 16, 1024, 2};
        f.writer.WriteFloat32Interleaved(view);

        const CounterSnapshot s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.visited, 16);
        CHECK_EQ_U64(ctx, s.written, 16);
        CHECK(ctx, Balanced(s));

        // Boundary frames in each of the three packets.
        CHECK(ctx, FrameMatches(f.bytes[1], 4, 2, 4, 2,
                                PcmSlotEncoding::Am824MBLA));
        CHECK(ctx, FrameMatches(f.bytes[1], 7, 2, 7, 2,
                                PcmSlotEncoding::Am824MBLA));
        CHECK(ctx, FrameMatches(f.bytes[2], 0, 2, 8, 2,
                                PcmSlotEncoding::Am824MBLA));
        CHECK(ctx, FrameMatches(f.bytes[2], 7, 2, 15, 2,
                                PcmSlotEncoding::Am824MBLA));
        CHECK(ctx, FrameMatches(f.bytes[3], 0, 2, 16, 2,
                                PcmSlotEncoding::Am824MBLA));
        CHECK(ctx, FrameMatches(f.bytes[3], 3, 2, 19, 2,
                                PcmSlotEncoding::Am824MBLA));

        // Frames 0..3 were not in the window: payload still zero.
        CHECK_EQ_U32(ctx, ReadSlotQuadlet(f.bytes[1], 0, 2, 0), 0);
        CHECK_EQ_U32(ctx, ReadSlotQuadlet(f.bytes[1], 3, 2, 1), 0);
    }

    // --- Partial overlap ahead: per-frame count-and-skip, no rejection ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(4)); // frames 0..23 exposed

        float host[16 * 2];
        FillHostWindow(host, 16, 16, 2);
        const HostAudioBufferView view{host, 16, 16, 1024, 2};
        f.writer.WriteFloat32Interleaved(view);

        const CounterSnapshot s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.visited, 16);
        CHECK_EQ_U64(ctx, s.written, 8);        // 16..23 covered
        CHECK_EQ_U64(ctx, s.withoutPacket, 8);  // 24..31 not produced yet
        CHECK_EQ_U64(ctx, s.outsidePacket, 0);
        CHECK(ctx, Balanced(s));

        // The covered half actually landed.
        CHECK(ctx, FrameMatches(f.bytes[3], 0, 2, 16, 2,
                                PcmSlotEncoding::Am824MBLA));
        CHECK(ctx, FrameMatches(f.bytes[3], 7, 2, 23, 2,
                                PcmSlotEncoding::Am824MBLA));
    }

    // --- Window entirely ahead (and against an empty timeline) ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));

        float host[8 * 2];
        FillHostWindow(host, 0, 8, 2);

        // Nothing exposed yet: everything is "no packet yet".
        const HostAudioBufferView early{host, 0, 8, 1024, 2};
        f.writer.WriteFloat32Interleaved(early);
        CounterSnapshot s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.withoutPacket, 8);
        CHECK_EQ_U64(ctx, s.outsidePacket, 0);

        CHECK(ctx, f.PrepareCycles(4)); // frames 0..23 exposed
        FillHostWindow(host, 100, 8, 2);
        const HostAudioBufferView far{host, 100, 8, 1024, 2};
        f.writer.WriteFloat32Interleaved(far);
        s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.visited, 16);
        CHECK_EQ_U64(ctx, s.withoutPacket, 16);
        CHECK_EQ_U64(ctx, s.outsidePacket, 0);
        CHECK(ctx, Balanced(s));
    }

    // --- Published slot is no longer writable: framesOutsidePacket ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(2)); // frames 0..7 exposed

        // Provider path takes the packet (→Published).
        f.timelineSlots[1].state.store(PacketSlotState::Published);

        float host[8 * 2];
        FillHostWindow(host, 0, 8, 2);
        const HostAudioBufferView view{host, 0, 8, 1024, 2};
        f.writer.WriteFloat32Interleaved(view);

        const CounterSnapshot s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.visited, 8);
        CHECK_EQ_U64(ctx, s.written, 0);
        CHECK_EQ_U64(ctx, s.withoutPacket, 0);
        CHECK_EQ_U64(ctx, s.outsidePacket, 8); // existed once, gone now
        CHECK(ctx, Balanced(s));
    }

    // --- Ring eviction: old frames are late, recent frames still land ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(40)); // 30 data packets, frames 0..239
        CHECK_EQ_U64(ctx, f.timeline.ExposedFrameEnd(), 240);

        float host[8 * 2];
        FillHostWindow(host, 0, 8, 2);
        const HostAudioBufferView old{host, 0, 8, 0, 2};
        f.writer.WriteFloat32Interleaved(old);
        CounterSnapshot s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.outsidePacket, 8); // evicted by ring reuse
        CHECK_EQ_U64(ctx, s.written, 0);

        FillHostWindow(host, 232, 8, 2);
        const HostAudioBufferView recent{host, 232, 8, 0, 2};
        f.writer.WriteFloat32Interleaved(recent);
        s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.written, 8);
        CHECK(ctx, Balanced(s));
    }

    // --- Host ring wrap: window crosses the end of the HAL ring buffer ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(4)); // frames 0..23 exposed

        constexpr uint32_t kRingFrames = 16;
        float ring[kRingFrames * 2];
        // Window = frames 12..19 living at ring offsets 12..15, then 0..3.
        for (uint32_t i = 0; i < 8; ++i) {
            const uint32_t pos = (12 + i) % kRingFrames;
            ring[pos * 2 + 0] = SampleFor(12 + i, 0);
            ring[pos * 2 + 1] = SampleFor(12 + i, 1);
        }
        const HostAudioBufferView view{&ring[12 * 2], 12, 8, kRingFrames, 2};
        f.writer.WriteFloat32Interleaved(view);

        const CounterSnapshot s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.visited, 8);
        CHECK_EQ_U64(ctx, s.written, 8);
        CHECK(ctx, Balanced(s));

        bool pcmOk = true;
        for (uint32_t i = 0; i < 4; ++i) { // frames 12..15 → packet 2
            pcmOk = pcmOk && FrameMatches(f.bytes[2], 4 + i, 2, 12 + i, 2,
                                          PcmSlotEncoding::Am824MBLA);
        }
        for (uint32_t i = 0; i < 4; ++i) { // frames 16..19 → packet 3
            pcmOk = pcmOk && FrameMatches(f.bytes[3], i, 2, 16 + i, 2,
                                          PcmSlotEncoding::Am824MBLA);
        }
        CHECK(ctx, pcmOk);
    }

    // --- Channel adaptation: short host frame zero-fills, wide one drops ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(2)); // frames 0..7

        float mono[8];
        for (uint32_t i = 0; i < 8; ++i) {
            mono[i] = SampleFor(i, 0);
        }
        const HostAudioBufferView view{mono, 0, 8, 1024, 1};
        f.writer.WriteFloat32Interleaved(view);

        CHECK_EQ_U64(ctx, Snap(f.writer.Counters()).written, 8);
        // ch0 = host sample; ch1 = encoded PCM zero (AM824 label only).
        CHECK_EQ_U32(ctx, ReadSlotQuadlet(f.bytes[1], 3, 2, 0),
                     PcmSlotCodec::EncodeFloat32(SampleFor(3, 0),
                                                 PcmSlotEncoding::Am824MBLA));
        CHECK_EQ_U32(ctx, ReadSlotQuadlet(f.bytes[1], 3, 2, 1), 0x40000000);
    }

    // --- Non-PCM (MIDI) slots keep the packetizer's defaults ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 1), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(2)); // dbs=3, frames 0..7

        float host[8 * 2];
        FillHostWindow(host, 0, 8, 2);
        const HostAudioBufferView view{host, 0, 8, 1024, 2};
        f.writer.WriteFloat32Interleaved(view);

        bool midiOk = true;
        bool pcmOk = true;
        for (uint32_t i = 0; i < 8; ++i) {
            midiOk = midiOk &&
                     (ReadSlotQuadlet(f.bytes[1], i, 3, 2) == 0x80000000u);
            pcmOk = pcmOk && FrameMatches(f.bytes[1], i, 3, i, 2,
                                          PcmSlotEncoding::Am824MBLA);
        }
        CHECK(ctx, midiOk);
        CHECK(ctx, pcmOk);
    }

    // --- Encoding follows the TX policy (Raw24-in-32 BE/LE) ---
    {
        for (const PcmSlotEncoding encoding :
             {PcmSlotEncoding::RawSigned24In32BE,
              PcmSlotEncoding::RawSigned24In32LE}) {
            WriterFixture f;
            AmdtpTxPolicy policy{};
            policy.hostToDevicePcmEncoding = encoding;
            CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), policy));
            CHECK(ctx, f.PrepareCycles(2));

            float host[8 * 2];
            FillHostWindow(host, 0, 8, 2);
            const HostAudioBufferView view{host, 0, 8, 1024, 2};
            f.writer.WriteFloat32Interleaved(view);

            bool pcmOk = true;
            for (uint32_t i = 0; i < 8; ++i) {
                pcmOk = pcmOk && FrameMatches(f.bytes[1], i, 2, i, 2, encoding);
            }
            CHECK(ctx, pcmOk);
        }
    }

    // --- Invalid views are no-ops: nothing visited, nothing counted ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));
        CHECK(ctx, f.PrepareCycles(2));

        float host[8 * 2];
        FillHostWindow(host, 0, 8, 2);

        f.writer.WriteFloat32Interleaved(
            HostAudioBufferView{nullptr, 0, 8, 1024, 2});
        f.writer.WriteFloat32Interleaved(
            HostAudioBufferView{host, 0, 0, 1024, 2});
        f.writer.WriteFloat32Interleaved(
            HostAudioBufferView{host, 0, 8, 1024, 0});

        AmdtpPayloadWriter unbound;
        unbound.Configure(f.packetizer.StreamConfig(), AmdtpTxPolicy{});
        unbound.WriteFloat32Interleaved(HostAudioBufferView{host, 0, 8, 1024, 2});

        CHECK_EQ_U64(ctx, Snap(f.writer.Counters()).visited, 0);
        CHECK_EQ_U64(ctx, Snap(unbound.Counters()).visited, 0);
    }

    // --- Long-run lockstep: packetizer and writer chase each other ---
    {
        WriterFixture f;
        CHECK(ctx, f.Setup(MakeConfig48kBlocking(2, 0), AmdtpTxPolicy{}));

        float host[24 * 2];
        uint64_t frame = 0;
        bool prepareOk = true;
        for (uint32_t iter = 0; iter < 25000; ++iter) {
            prepareOk = prepareOk && f.PrepareCycles(4); // exposes 24 frames
            FillHostWindow(host, frame, 24, 2);
            const HostAudioBufferView view{host, frame, 24, 0, 2};
            f.writer.WriteFloat32Interleaved(view);
            frame += 24;
        }
        CHECK(ctx, prepareOk);

        const CounterSnapshot s = Snap(f.writer.Counters());
        CHECK_EQ_U64(ctx, s.visited, 600000);
        CHECK_EQ_U64(ctx, s.written, 600000);
        CHECK_EQ_U64(ctx, s.withoutPacket, 0);
        CHECK_EQ_U64(ctx, s.outsidePacket, 0);
        CHECK(ctx, Balanced(s));
        CHECK_EQ_U64(ctx, f.timeline.ExposedFrameEnd(), 600000);
    }
}

} // namespace ASFW::LabTests
