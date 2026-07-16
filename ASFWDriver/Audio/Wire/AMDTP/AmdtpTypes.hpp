#pragma once

#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

enum class StreamMode : uint8_t {
    Blocking = 0,
    NonBlocking = 1,
};

enum class PcmSlotEncoding : uint8_t {
    Am824MBLA = 0,
    RawSigned24In32BE = 1,
    RawSigned24In32LE = 2,
};

enum class DbsPolicy : uint8_t {
    Constant = 0,
    VariablePerPacket = 1,
};

struct AmdtpStreamConfig final {
    uint32_t sampleRate{48000};
    StreamMode streamMode{StreamMode::Blocking};

    uint8_t sid{0};
    uint8_t dbs{0};
    uint8_t pcmChannels{0};
    uint8_t midiSlots{0};

    uint8_t fmt{0x10};
    uint8_t fdf{0x02};

    uint8_t framesPerDataPacket{8};
    uint32_t maxPacketBytes{512};

    // First host buffer channel this stream encodes. For a multi-stream device
    // (Venice F32 = 2×16) the 32-ch host output buffer is split across streams:
    // stream 0 reads channels [0, pcmChannels), stream 1 reads [16, 16+pcmChannels),
    // etc. Single-stream devices keep 0.
    uint8_t sourceChannelOffset{0};
};

struct AmdtpTxPolicy final {
    PcmSlotEncoding hostToDevicePcmEncoding{PcmSlotEncoding::Am824MBLA};
    DbsPolicy dbsPolicy{DbsPolicy::Constant};

    uint32_t defaultNonAudioSlotWord{0x80000000};
    bool clearPayloadBeforeExposure{true};
    bool initializeNonAudioSlots{true};
    bool preserveFdfInNoDataPackets{false};
    bool emptyPacketsDuringIdle{false};
};

struct HostAudioBufferView final {
    const float* interleavedFloat32{nullptr};

    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t frameCapacity{0};
    uint32_t channels{0};
};

struct TxPacketSlotView final {
    uint32_t packetIndex{0};
    uint8_t* bytes{nullptr};
    uint32_t capacityBytes{0};
};

struct PreparedTxPacket final {
    uint32_t packetIndex{0};
    uint32_t byteCount{0};

    bool isData{false};
    uint8_t dbc{0};
    uint16_t syt{0xFFFF};

    uint64_t firstAudioFrame{0};
    uint32_t framesInPacket{0};
    uint32_t dbs{0};
};

enum class AmdtpPacketDisposition : uint8_t {
    NoData = 0,
    Data = 1,
};

struct AmdtpTimingState final {
    int64_t timelineEpochTicks{0};
    int64_t nowTicks{0};

    bool txClockValid{false};
    AmdtpPacketDisposition disposition{
        AmdtpPacketDisposition::NoData};
    uint16_t nextDataSyt{0xFFFF};
    uint16_t replayDataBlocks{0};
    bool replayValid{false};
    uint64_t nextAudioFrame{0};
};

} // namespace ASFW::Protocols::Audio::AMDTP

namespace ASFW::Encoding {

enum class StreamMode : uint8_t {
    kNonBlocking = 0,
    kBlocking = 1,
};

enum class AudioWireFormat : uint8_t {
    kAM824 = 0,
    kRawPcm24In32 = 1,
};

} // namespace ASFW::Encoding
