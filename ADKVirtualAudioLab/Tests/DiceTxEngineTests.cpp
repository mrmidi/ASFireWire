#include "TestHarness.hpp"

#include "../Core/VirtualAudioDeviceController.hpp"
#include "../Lab/FakeIsochTxSlotProvider.hpp"
#include "../Protocols/Audio/AMDTP/PcmSlotCodec.hpp"
#include "../Protocols/Audio/DICE/DiceProfileRegistry.hpp"
#include "../Protocols/Audio/DICE/DiceStreamConfig.hpp"
#include "../Protocols/Audio/DICE/DiceTxStreamEngine.hpp"
#include "../Protocols/Audio/DICE/Profiles/FocusriteSaffireProfile.hpp"
#include "../Protocols/Audio/DICE/Profiles/GenericDiceProfile.hpp"

namespace ASFW::LabTests {

using namespace Protocols::Audio;
using namespace Protocols::Audio::DICE;

namespace {

constexpr DiceDeviceIdentity kFocusriteIdentity{0x130E01020304ULL, 0x00130E, 1};
constexpr DiceDeviceIdentity kUnknownIdentity{0xBEEF000000000000ULL, 0xBEEF, 7};

float SampleFor(uint64_t frame, uint32_t channel) {
    const float base = static_cast<float>(frame % 32) / 32.0f;
    return (channel % 2 == 0) ? base : -base;
}

void FillHostWindow(float* host, uint64_t firstFrame, uint32_t frameCount,
                    uint32_t channels) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            host[i * channels + ch] = SampleFor(firstFrame + i, ch);
        }
    }
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
                  AMDTP::PcmSlotEncoding encoding) {
    for (uint32_t ch = 0; ch < pcmChannels; ++ch) {
        const uint32_t expected = AMDTP::PcmSlotCodec::EncodeFloat32(
            SampleFor(absoluteFrame, ch), encoding);
        if (ReadSlotQuadlet(packetBytes, frameInPacket, dbs, ch) != expected) {
            return false;
        }
    }
    return true;
}

// Slot provider that refuses every acquire — for the failure-counter path.
class RefusingSlotProvider final : public AMDTP::IAmdtpTxSlotProvider {
public:
    bool AcquireWritableSlot(uint32_t,
                             AMDTP::TxPacketSlotView&) noexcept override {
        return false;
    }
    void PublishSlot(const AMDTP::PreparedTxPacket&) noexcept override {}
    uint32_t SlotCount() const noexcept override { return 0; }
};

struct EngineFixture final {
    Profiles::FocusriteSaffireProfile profile{};
    DiceTxStreamEngine engine{};
    Lab::FakeIsochTxSlotProvider provider{};

    bool Setup() {
        DiceStreamConfig txConfig{};
        if (!profile.BuildDefaultTxStreamConfig(txConfig)) {
            return false;
        }
        // Pin a stereo, MIDI-free shape: these tests assert byte-exact CIP
        // and tiling images. The profile's bench shape (8+1, dbs 9) is
        // covered by the profile and controller end-to-end tests.
        txConfig.pcmChannels = 2;
        txConfig.midiSlots = 0;
        txConfig.dbs = 2;
        if (!engine.Configure(profile, txConfig)) {
            return false;
        }
        engine.BindSlotProvider(&provider);
        engine.ResetForStart(0, 0);
        return true;
    }

    bool PreparePackets(uint32_t firstIndex, uint32_t count,
                        const AMDTP::AmdtpTimingState& timing) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!engine.PrepareNextTransmitSlot(firstIndex + i, timing)) {
                return false;
            }
        }
        return true;
    }
};

} // namespace

void RunDiceTxEngineTests(TestContext& ctx) {
    // --- DiceStreamConfig → AmdtpStreamConfig mapping ---
    {
        DiceStreamConfig dice{};
        dice.sampleRate = 48000;
        dice.streamMode = AMDTP::StreamMode::Blocking;
        dice.sid = 3;
        dice.pcmChannels = 4;
        dice.dbs = 5;
        dice.midiSlots = 1;
        dice.framesPerDataPacket = 8;
        dice.fdf = 0x02;
        dice.fmt = 0x10;

        const AMDTP::AmdtpStreamConfig amdtp =
            DiceStreamConfigMapper::ToAmdtpConfig(dice);
        CHECK_EQ_U32(ctx, amdtp.sampleRate, 48000);
        CHECK(ctx, amdtp.streamMode == AMDTP::StreamMode::Blocking);
        CHECK_EQ_U32(ctx, amdtp.sid, 3);
        CHECK_EQ_U32(ctx, amdtp.pcmChannels, 4);
        CHECK_EQ_U32(ctx, amdtp.dbs, 5);
        CHECK_EQ_U32(ctx, amdtp.midiSlots, 1);
        CHECK_EQ_U32(ctx, amdtp.framesPerDataPacket, 8);
        CHECK_EQ_U32(ctx, amdtp.fdf, 0x02);
        CHECK_EQ_U32(ctx, amdtp.fmt, 0x10);
        CHECK_EQ_U32(ctx, amdtp.maxPacketBytes, 512);
    }

    // --- Profiles: matching and quirks ---
    {
        Profiles::FocusriteSaffireProfile saffire;
        Profiles::GenericDiceProfile generic;

        CHECK(ctx, saffire.Matches(kFocusriteIdentity));
        CHECK(ctx, !saffire.Matches(kUnknownIdentity));
        CHECK(ctx, generic.Matches(kFocusriteIdentity));
        CHECK(ctx, generic.Matches(kUnknownIdentity));

        const DiceDeviceQuirks saffireQuirks = saffire.Quirks();
        CHECK(ctx, saffireQuirks.tx.hostToDevicePcmEncoding ==
                       AMDTP::PcmSlotEncoding::RawSigned24In32BE);
        CHECK(ctx, saffireQuirks.rx.deviceToHostPcmEncoding ==
                       AMDTP::PcmSlotEncoding::Am824MBLA);

        const DiceDeviceQuirks genericQuirks = generic.Quirks();
        CHECK(ctx, genericQuirks.tx.hostToDevicePcmEncoding ==
                       AMDTP::PcmSlotEncoding::Am824MBLA);

        // Bench shapes from the Saffire.kext wire capture: host→device
        // dbs 9 (8 audio + 1 MIDI), device→host dbs 17 (16 audio + 1 MIDI).
        DiceStreamConfig tx{};
        CHECK(ctx, saffire.BuildDefaultTxStreamConfig(tx));
        CHECK(ctx, tx.direction == DiceStreamDirection::HostToDevice);
        CHECK_EQ_U32(ctx, tx.sampleRate, 48000);
        CHECK_EQ_U32(ctx, tx.pcmChannels, 8);
        CHECK_EQ_U32(ctx, tx.midiSlots, 1);
        CHECK_EQ_U32(ctx, tx.dbs, 9);
        CHECK_EQ_U32(ctx, tx.framesPerDataPacket, 8);

        DiceStreamConfig rx{};
        CHECK(ctx, saffire.BuildDefaultRxStreamConfig(rx));
        CHECK(ctx, rx.direction == DiceStreamDirection::DeviceToHost);
        CHECK_EQ_U32(ctx, rx.pcmChannels, 16);
        CHECK_EQ_U32(ctx, rx.midiSlots, 1);
        CHECK_EQ_U32(ctx, rx.dbs, 17);
    }

    // --- Registry: lookup and generic fallback ---
    {
        Profiles::FocusriteSaffireProfile saffire;
        DiceProfileRegistry registry;

        CHECK(ctx, !registry.RegisterProfile(nullptr));
        CHECK(ctx, registry.RegisterProfile(&saffire));
        CHECK_EQ_U32(ctx, registry.ProfileCount(), 1);

        CHECK(ctx, registry.FindProfile(kFocusriteIdentity) == &saffire);
        CHECK(ctx, registry.FindProfile(kUnknownIdentity) == nullptr);

        const IDiceDeviceProfile* generic = registry.GenericProfile();
        CHECK(ctx, generic != nullptr);
        CHECK(ctx, generic->Matches(kUnknownIdentity));
    }

    // --- Engine: configure validation ---
    {
        EngineFixture f;
        DiceStreamConfig rxConfig{};
        CHECK(ctx, f.profile.BuildDefaultRxStreamConfig(rxConfig));
        CHECK(ctx, !f.engine.Configure(f.profile, rxConfig)); // wrong direction

        DiceStreamConfig badRate{};
        CHECK(ctx, f.profile.BuildDefaultTxStreamConfig(badRate));
        badRate.sampleRate = 44100;
        CHECK(ctx, !f.engine.Configure(f.profile, badRate)); // honest failure
    }

    // --- Engine: prepare → publish → fill, byte-exact on the fake ring ---
    {
        EngineFixture f;
        CHECK(ctx, f.Setup());

        const AMDTP::AmdtpTimingState noClock{};
        CHECK(ctx, f.PreparePackets(0, 4, noClock)); // N, D, D, D

        const DiceTxEngineCounters& counters = f.engine.Counters();
        CHECK_EQ_U64(ctx, counters.packetsPrepared.load(), 4);
        CHECK_EQ_U64(ctx, counters.dataPacketsPrepared.load(), 3);
        CHECK_EQ_U64(ctx, counters.noDataPacketsPrepared.load(), 1);
        CHECK_EQ_U64(ctx, counters.slotAcquireFailures.load(), 0);

        // Published metadata: cycle 0 no-data (8 bytes, DBC unchanged),
        // cycles 1-3 data tiling frames 0..23.
        const AMDTP::PreparedTxPacket* noData = f.provider.PublishedPacket(0);
        CHECK(ctx, noData != nullptr && !noData->isData);
        CHECK_EQ_U32(ctx, noData->byteCount, 8);
        const AMDTP::PreparedTxPacket* data1 = f.provider.PublishedPacket(1);
        CHECK(ctx, data1 != nullptr && data1->isData);
        CHECK_EQ_U64(ctx, data1->firstAudioFrame, 0);
        CHECK_EQ_U32(ctx, data1->framesInPacket, 8);
        const AMDTP::PreparedTxPacket* data3 = f.provider.PublishedPacket(3);
        CHECK(ctx, data3 != nullptr && data3->firstAudioFrame == 16);

        // CIP image in provider storage: sid=0 dbs=2 dbc=0, FDF=0x02,
        // SYT=0xFFFF while the TX clock is invalid.
        const uint8_t* packet1 = f.provider.SlotBytes(1);
        const uint8_t expectedCip[8] = {0x00, 0x02, 0x00, 0x00,
                                        0x90, 0x02, 0xFF, 0xFF};
        bool cipOk = true;
        for (int i = 0; i < 8; ++i) {
            cipOk = cipOk && (packet1[i] == expectedCip[i]);
        }
        CHECK(ctx, cipOk);

        // Host PCM written after publish lands in the published bytes
        // (publish-at-prepare keeps the storage live).
        float host[24 * 2];
        FillHostWindow(host, 0, 24, 2);
        const AMDTP::HostAudioBufferView view{host, 0, 24, 0, 2};
        f.engine.WriteHostOutputFloat32(view);

        bool pcmOk = true;
        for (uint32_t i = 0; i < 8; ++i) {
            pcmOk = pcmOk &&
                    FrameMatches(f.provider.SlotBytes(1), i, 2, i, 2,
                                 AMDTP::PcmSlotEncoding::RawSigned24In32BE);
            pcmOk = pcmOk &&
                    FrameMatches(f.provider.SlotBytes(2), i, 2, 8 + i, 2,
                                 AMDTP::PcmSlotEncoding::RawSigned24In32BE);
            pcmOk = pcmOk &&
                    FrameMatches(f.provider.SlotBytes(3), i, 2, 16 + i, 2,
                                 AMDTP::PcmSlotEncoding::RawSigned24In32BE);
        }
        CHECK(ctx, pcmOk);

        // Raw 24-in-32 means no AM824 label: top byte of a nonzero sample
        // quadlet stays 0x00.
        const uint32_t q = ReadSlotQuadlet(f.provider.SlotBytes(1), 1, 2, 0);
        CHECK(ctx, q != 0 && (q >> 24) == 0x00);

        // Valid TX clock stamps the SYT into the CIP (cycle 4 = N, 5 = D).
        AMDTP::AmdtpTimingState clock{};
        clock.txClockValid = true;
        clock.nextDataSyt = 0x1234;
        CHECK(ctx, f.PreparePackets(4, 2, clock));
        const uint8_t* packet5 = f.provider.SlotBytes(5);
        CHECK_EQ_U32(ctx, packet5[6], 0x12);
        CHECK_EQ_U32(ctx, packet5[7], 0x34);
    }

    // --- Engine: acquire failure is counted, prepare state not advanced ---
    {
        EngineFixture f;
        CHECK(ctx, f.Setup());

        RefusingSlotProvider refusing;
        f.engine.BindSlotProvider(&refusing);
        const AMDTP::AmdtpTimingState noClock{};
        CHECK(ctx, !f.engine.PrepareNextTransmitSlot(0, noClock));
        CHECK_EQ_U64(ctx, f.engine.Counters().slotAcquireFailures.load(), 1);

        // Rebinding the real provider: cadence did not advance, so cycle 0
        // is still the no-data cycle.
        f.engine.BindSlotProvider(&f.provider);
        CHECK(ctx, f.engine.PrepareNextTransmitSlot(0, noClock));
        const AMDTP::PreparedTxPacket* packet = f.provider.PublishedPacket(0);
        CHECK(ctx, packet != nullptr && !packet->isData);
    }

    // --- Fake provider: ring reuse evicts the previous publication ---
    {
        Lab::FakeIsochTxSlotProvider provider;
        AMDTP::TxPacketSlotView slot{};
        CHECK(ctx, provider.AcquireWritableSlot(0, slot));
        AMDTP::PreparedTxPacket packet{};
        packet.packetIndex = 0;
        provider.PublishSlot(packet);
        CHECK(ctx, provider.PublishedPacket(0) != nullptr);
        const uint32_t count = provider.SlotCount();
        CHECK(ctx, provider.PublishedPacket(count) == nullptr); // same ring pos

        CHECK(ctx, provider.AcquireWritableSlot(count, slot)); // reuse evicts
        CHECK(ctx, provider.PublishedPacket(0) == nullptr);
    }

    // --- Controller end-to-end: Saffire profile, Raw24 on the wire ---
    {
        Driver::VirtualAudioDeviceController controller;
        CHECK(ctx, controller.Initialize());
        CHECK(ctx, controller.SelectProfile(kFocusriteIdentity));

        // The HAL shape comes from the profile via caps (8 PCM channels;
        // the MIDI slot is wire-only).
        Driver::OutputDeviceCaps caps{};
        CHECK(ctx, controller.GetOutputDeviceCaps(caps));
        CHECK_EQ_U32(ctx, caps.sampleRate, 48000);
        CHECK_EQ_U32(ctx, caps.pcmChannels, 8);

        CHECK(ctx, !controller.ConfigureOutputStream(44100, 8, 512)); // honest
        CHECK(ctx, !controller.ConfigureOutputStream(48000, 0, 512));
        CHECK(ctx, controller.ConfigureOutputStream(caps.sampleRate,
                                                    caps.pcmChannels, 512));
        controller.ResetTransportLab(0, 0);

        bool prepared = true;
        for (uint32_t idx = 0; idx < 4; ++idx) {
            prepared = prepared && controller.PrepareLabPacket(idx, 0xFFFF, false);
        }
        CHECK(ctx, prepared);

        float host[24 * 8];
        FillHostWindow(host, 0, 24, 8);
        const AMDTP::HostAudioBufferView view{host, 0, 24, 512, 8};
        controller.SubmitWriteEnd(view);

        // Wire shape per the Saffire.kext capture: dbs 9, 296-byte data
        // packets, raw sign-extended PCM, empty MIDI slot = 0x80000000.
        const Lab::FakeIsochTxSlotProvider& fake = controller.FakeSlotProvider();
        const AMDTP::PreparedTxPacket* data1 = fake.PublishedPacket(1);
        CHECK(ctx, data1 != nullptr && data1->isData);
        CHECK_EQ_U32(ctx, data1->dbs, 9);
        CHECK_EQ_U32(ctx, data1->byteCount, 8 + 8 * 9 * 4);
        bool pcmOk = true;
        bool midiOk = true;
        for (uint32_t i = 0; i < 8; ++i) {
            pcmOk = pcmOk &&
                    FrameMatches(fake.SlotBytes(1), i, 9, i, 8,
                                 AMDTP::PcmSlotEncoding::RawSigned24In32BE);
            midiOk = midiOk &&
                     (ReadSlotQuadlet(fake.SlotBytes(1), i, 9, 8) == 0x80000000u);
        }
        CHECK(ctx, pcmOk);
        CHECK(ctx, midiOk);
    }

    // --- Controller end-to-end: unknown device → generic profile → AM824 ---
    {
        Driver::VirtualAudioDeviceController controller;
        CHECK(ctx, controller.Initialize());
        CHECK(ctx, controller.SelectProfile(kUnknownIdentity)); // falls back
        CHECK(ctx, controller.ConfigureOutputStream(48000, 2, 512));
        controller.ResetTransportLab(0, 0);

        bool prepared = true;
        for (uint32_t idx = 0; idx < 2; ++idx) {
            prepared = prepared && controller.PrepareLabPacket(idx, 0xFFFF, false);
        }
        CHECK(ctx, prepared);

        float host[8 * 2];
        FillHostWindow(host, 0, 8, 2);
        const AMDTP::HostAudioBufferView view{host, 0, 8, 512, 2};
        controller.SubmitWriteEnd(view);

        const Lab::FakeIsochTxSlotProvider& fake = controller.FakeSlotProvider();
        bool pcmOk = true;
        for (uint32_t i = 0; i < 8; ++i) {
            pcmOk = pcmOk && FrameMatches(fake.SlotBytes(1), i, 2, i, 2,
                                          AMDTP::PcmSlotEncoding::Am824MBLA);
        }
        CHECK(ctx, pcmOk);
        // AM824 label present on every PCM quadlet.
        CHECK_EQ_U32(ctx, ReadSlotQuadlet(fake.SlotBytes(1), 0, 2, 0) >> 24,
                     0x40);
    }
}

} // namespace ASFW::LabTests
