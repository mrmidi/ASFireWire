#include <AudioDriverKit/AudioDriverKit.h>

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "AudioEngine/Direct/DirectOutputReader.hpp"
#include "AudioEngine/Direct/Tx/DirectTxPacketScratch.hpp"
#include "AudioEngine/Direct/Tx/TxAudioPacketWriter.hpp"
#include "AudioWire/AM824/AM824Encoder.hpp"
#include "Isoch/Transmit/TxVerifierDecode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace ASFW::Tests::AudioEngineDirect {

using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioStreamMode;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::AudioWireFormat;
using ASFW::AudioEngine::Direct::DirectOutputReader;
using ASFW::AudioEngine::Direct::Tx::DirectTxReadStatus;
using ASFW::AudioEngine::Direct::Tx::kDirectTxCipHeaderBytes;
using ASFW::AudioEngine::Direct::Tx::TxAudioPacketWriteRequest;
using ASFW::AudioEngine::Direct::Tx::TxAudioPacketWriter;
using ASFW::Audio::Runtime::TxPacketState;
using ASFW::Audio::Runtime::TxBlockingResult;
using ASFW::Isoch::TxVerify::ParseCIPFromHostWords;

AudioGraphBinding MakeWriterOutputBinding(AudioTransportControlBlock& control,
                                          IOUserAudioDevice& audioDevice,
                                          const int32_t* output,
                                          uint32_t frameCapacity,
                                          uint32_t channels,
                                          uint32_t am824Slots = 0) {
    const uint32_t slots = am824Slots == 0 ? channels : am824Slots;
    return AudioGraphBinding{
        .guid = 0x1122334455667788ULL,
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .outputBase = output,
            .outputFrameCapacity = frameCapacity,
            .outputChannels = channels,
        },
        .control = &control,
        .hostToDeviceAm824Slots = slots,
        .streamMode = AudioStreamMode::kBlocking,
        .hostToDeviceWireFormat = AudioWireFormat::kAM824,
        .audioDevice = &audioDevice,
    };
}

uint32_t PacketWordAt(const std::array<uint8_t, 128>& packet, uint32_t byteOffset) {
    uint32_t word = 0;
    std::memcpy(&word, packet.data() + byteOffset, sizeof(word));
    return word;
}

bool PacketIsFilledWith(const std::array<uint8_t, 128>& packet, uint8_t value) {
    for (const uint8_t byte : packet) {
        if (byte != value) {
            return false;
        }
    }
    return true;
}

static void PublishPlaybackWriteEnd(AudioTransportControlBlock& control,
                                    uint64_t sampleFrame,
                                    uint64_t hostTime,
                                    uint32_t frameCount) {
    control.client.PublishWriteEnd(sampleFrame, hostTime, frameCount);
    control.playbackRingWriteFrame.store(control.client.OutputWrittenEndFrame(),
                                         std::memory_order_release);
}

TEST(TxAudioPacketWriterTests, DataAvailableWritesRealPcm) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    output[4] = 0x00123456;
    output[5] = static_cast<int32_t>(0x00FEDCBA);
    output[6] = 0x00000056;
    output[7] = static_cast<int32_t>(0x00E55654);
    auto binding = MakeWriterOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    TxAudioPacketWriter writer(reader);
    std::array<uint8_t, 128> packet{};

    PublishPlaybackWriteEnd(control, 2, 0, 2);

    const auto result = writer.WritePacket(TxAudioPacketWriteRequest{
        .firstFrame = 2,
        .frameCount = 2,
        .channels = 2,
        .sid = 0x02,
        .dbc = 0xC0,
        .syt = 0x79FE,
        .dataPacket = true,
    }, packet.data(), static_cast<uint32_t>(packet.size()));

    EXPECT_EQ(result.state, TxPacketState::ValidPhasePcm);
    EXPECT_EQ(result.blockingResult, TxBlockingResult::Data);
    EXPECT_EQ(result.quadlets * 4U, kDirectTxCipHeaderBytes + (2U * 2U * 4U));
    EXPECT_EQ(result.frames, 2U);
    EXPECT_FALSE(result.fatal);

    const auto cip = ParseCIPFromHostWords(PacketWordAt(packet, 0), PacketWordAt(packet, 4));
    EXPECT_EQ(cip.sid, 0x02);
    EXPECT_EQ(cip.dbs, 2);
    EXPECT_EQ(cip.dbc, 0xC0);
    EXPECT_EQ(cip.fmt, 0x10);
    EXPECT_EQ(cip.fdf, 0x02);
    EXPECT_EQ(cip.syt, 0x79FE);

    EXPECT_EQ(PacketWordAt(packet, 8), ASFW::Encoding::AM824Encoder::encode(output[4]));
    EXPECT_EQ(PacketWordAt(packet, 12), ASFW::Encoding::AM824Encoder::encode(output[5]));
    EXPECT_EQ(PacketWordAt(packet, 16), ASFW::Encoding::AM824Encoder::encode(output[6]));
    EXPECT_EQ(PacketWordAt(packet, 20), ASFW::Encoding::AM824Encoder::encode(output[7]));
}

TEST(TxAudioPacketWriterTests, DataUnavailableWritesSilenceDataPacket) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeWriterOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    TxAudioPacketWriter writer(reader);
    std::array<uint8_t, 128> packet{};

    const auto result = writer.WritePacket(TxAudioPacketWriteRequest{
        .firstFrame = 0,
        .frameCount = 2,
        .channels = 2,
        .sid = 0x02,
        .dbc = 0xC0,
        .syt = 0x79FE,
        .dataPacket = true,
    }, packet.data(), static_cast<uint32_t>(packet.size()));

    EXPECT_EQ(result.state, TxPacketState::UnderrunSilence);
    EXPECT_EQ(result.blockingResult, TxBlockingResult::Data);
    EXPECT_EQ(result.quadlets * 4U, kDirectTxCipHeaderBytes + (2U * 2U * 4U));
    EXPECT_EQ(result.frames, 2U);
    EXPECT_FALSE(result.fatal);

    const auto cip = ParseCIPFromHostWords(PacketWordAt(packet, 0), PacketWordAt(packet, 4));
    EXPECT_EQ(cip.syt, 0x79FE);

    const uint32_t silence = ASFW::Encoding::AM824Encoder::encodeSilence();
    EXPECT_EQ(PacketWordAt(packet, 8), silence);
    EXPECT_EQ(PacketWordAt(packet, 12), silence);
    EXPECT_EQ(PacketWordAt(packet, 16), silence);
    EXPECT_EQ(PacketWordAt(packet, 20), silence);
}

TEST(TxAudioPacketWriterTests, NoDataWritesOnlyCipHeader) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeWriterOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    TxAudioPacketWriter writer(reader);
    std::array<uint8_t, 128> packet{};

    const auto result = writer.WritePacket(TxAudioPacketWriteRequest{
        .frameCount = 0,
        .channels = 2,
        .sid = 0x02,
        .dbc = 0xC0,
        .syt = 0x1234,
        .dataPacket = false,
    }, packet.data(), static_cast<uint32_t>(packet.size()));

    EXPECT_EQ(result.state, TxPacketState::ValidPhaseSilence);
    EXPECT_EQ(result.blockingResult, TxBlockingResult::NoData);
    EXPECT_EQ(result.quadlets * 4U, kDirectTxCipHeaderBytes);
    EXPECT_EQ(result.frames, 0U);
    EXPECT_FALSE(result.fatal);

    const auto cip = ParseCIPFromHostWords(PacketWordAt(packet, 0), PacketWordAt(packet, 4));
    EXPECT_EQ(cip.sid, 0x02);
    EXPECT_EQ(cip.dbs, 2);
    EXPECT_EQ(cip.dbc, 0xC0);
    EXPECT_EQ(cip.syt, 0xFFFF);
}

TEST(TxAudioPacketWriterTests, CapacityTooSmallFailsWithoutTouchingPacket) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 16> output{};
    auto binding = MakeWriterOutputBinding(control, audioDevice, output.data(), 8, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    TxAudioPacketWriter writer(reader);
    std::array<uint8_t, 128> packet{};
    packet.fill(0xA5);

    PublishPlaybackWriteEnd(control, 0, 0, 2);

    const auto result = writer.WritePacket(TxAudioPacketWriteRequest{
        .firstFrame = 0,
        .frameCount = 2,
        .channels = 2,
        .sid = 0x02,
        .dbc = 0xC0,
        .syt = 0x79FE,
        .dataPacket = true,
    }, packet.data(), 12);

    EXPECT_EQ(result.state, TxPacketState::InvalidGeometry);
    EXPECT_TRUE(result.fatal);
    EXPECT_EQ(result.quadlets * 4U, 0U);
    EXPECT_EQ(result.frames, 0U);
    EXPECT_TRUE(PacketIsFilledWith(packet, 0xA5));
}

TEST(TxAudioPacketWriterTests, FrameWrapUsesModuloMemoryFrames) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 8> output{};
    output[6] = 0x00111111;
    output[7] = 0x00222222;
    output[0] = 0x00333333;
    output[1] = 0x00444444;
    auto binding = MakeWriterOutputBinding(control, audioDevice, output.data(), 4, 2);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    TxAudioPacketWriter writer(reader);
    std::array<uint8_t, 128> packet{};

    PublishPlaybackWriteEnd(control, 3, 0, 2);

    const auto result = writer.WritePacket(TxAudioPacketWriteRequest{
        .firstFrame = 3,
        .frameCount = 2,
        .channels = 2,
        .sid = 0x02,
        .dbc = 0xC0,
        .syt = 0x79FE,
        .dataPacket = true,
    }, packet.data(), static_cast<uint32_t>(packet.size()));

    EXPECT_EQ(result.state, TxPacketState::ValidPhasePcm);
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(PacketWordAt(packet, 8), ASFW::Encoding::AM824Encoder::encode(output[6]));
    EXPECT_EQ(PacketWordAt(packet, 12), ASFW::Encoding::AM824Encoder::encode(output[7]));
    EXPECT_EQ(PacketWordAt(packet, 16), ASFW::Encoding::AM824Encoder::encode(output[0]));
    EXPECT_EQ(PacketWordAt(packet, 20), ASFW::Encoding::AM824Encoder::encode(output[1]));
}

TEST(TxAudioPacketWriterTests, ExtraAm824SlotsUseMidiPlaceholders) {
    AudioTransportControlBlock control{};
    IOUserAudioDevice audioDevice{};
    std::array<int32_t, 8> output{};
    output[0] = 0x00111111;
    output[1] = 0x00222222;
    auto binding = MakeWriterOutputBinding(control, audioDevice, output.data(), 4, 2, 3);
    DirectOutputReader reader{};
    reader.Bind(&binding);
    TxAudioPacketWriter writer(reader);
    std::array<uint8_t, 128> packet{};

    PublishPlaybackWriteEnd(control, 0, 0, 1);

    const auto result = writer.WritePacket(TxAudioPacketWriteRequest{
        .firstFrame = 0,
        .frameCount = 1,
        .channels = 2,
        .am824Slots = 3,
        .sid = 0x02,
        .dbc = 0xC0,
        .syt = 0x79FE,
        .dataPacket = true,
    }, packet.data(), static_cast<uint32_t>(packet.size()));

    EXPECT_EQ(result.state, TxPacketState::ValidPhasePcm);
    EXPECT_EQ(result.quadlets * 4U, kDirectTxCipHeaderBytes + (1U * 3U * 4U));
    EXPECT_FALSE(result.fatal);

    const auto cip = ParseCIPFromHostWords(PacketWordAt(packet, 0), PacketWordAt(packet, 4));
    EXPECT_EQ(cip.dbs, 3);
    EXPECT_EQ(PacketWordAt(packet, 8), ASFW::Encoding::AM824Encoder::encode(output[0]));
    EXPECT_EQ(PacketWordAt(packet, 12), ASFW::Encoding::AM824Encoder::encode(output[1]));
    EXPECT_EQ(PacketWordAt(packet, 16), ASFW::Encoding::AM824Encoder::encodeLabelOnly(0x80));
}

TEST(TxAudioPacketWriterTests, InvalidBindingFailsWithoutTouchingPacket) {
    DirectOutputReader reader{};
    TxAudioPacketWriter writer(reader);
    std::array<uint8_t, 128> packet{};
    packet.fill(0x5A);

    const auto result = writer.WritePacket(TxAudioPacketWriteRequest{
        .firstFrame = 0,
        .frameCount = 1,
        .channels = 2,
        .sid = 0x02,
        .dbc = 0xC0,
        .syt = 0x79FE,
        .dataPacket = true,
    }, packet.data(), static_cast<uint32_t>(packet.size()));

    EXPECT_EQ(result.state, TxPacketState::InvalidGeometry);
    EXPECT_TRUE(result.fatal);
    EXPECT_EQ(result.quadlets * 4U, 0U);
    EXPECT_EQ(result.frames, 0U);
    EXPECT_TRUE(PacketIsFilledWith(packet, 0x5A));
}

} // namespace ASFW::Tests::AudioEngineDirect
