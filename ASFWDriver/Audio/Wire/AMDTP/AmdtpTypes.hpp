#pragma once

#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

/// WARNING: this enum's numeric values are INVERTED relative to every other
/// StreamMode in the tree — ASFW::Encoding::StreamMode (below, same file),
/// Audio::Model::StreamMode, and Isoch::Audio::StreamMode all use
/// kNonBlocking = 0 / kBlocking = 1, and the nub wire field documents
/// 0 = non-blocking, 1 = blocking.
///
/// Never convert to or from those by cast or std::to_underlying: a numeric
/// conversion silently turns blocking into non-blocking. The one crossing point
/// maps by name (DiceTxStreamEngine.cpp ToAmdtpConfig) and must stay that way.
/// This matters for devices whose profile demands blocking transmission — see
/// the Apogee Duet quirk in ApogeeCaps.hpp (FW-140).
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

// Most AMDTP devices hold DBC on a header-only NO-DATA packet.  M-Audio's
// special 1814 / ProjectMix firmware is the narrow exception: its output path
// advances by SYT_INTERVAL even when no data blocks are carried.
enum class NoDataDbcPolicy : uint8_t {
    Hold = 0,
    AdvanceBySytInterval = 1,
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
    bool initializeNonAudioSlots{true};
    bool preserveFdfInNoDataPackets{false};
    bool emptyPacketsDuringIdle{false};
    NoDataDbcPolicy noDataDbcPolicy{NoDataDbcPolicy::Hold};
};

// Value-owned PCM snapshot supplied to the packetizer. The bytes referenced by
// this view live in the TX engine's private scratch storage, never in the HAL
// ring and never in an already-committed transport slot.
struct TxPcmSnapshotView final {
    const float* interleavedFloat32{nullptr};
    uint32_t frameCount{0};
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

    // Load-bearing publication invariant. Every DATA packet must have copied a
    // complete, stable PCM snapshot before IAmdtpTxSlotProvider::PublishSlot.
    bool pcmFinalized{false};
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

    // A device-specific timing policy may provide an explicit per-packet
    // decision without borrowing RX replay.  This is intentionally a generic
    // packetizer seam: it carries no M-Audio identity or transport detail.
    bool hasExplicitPacketSchedule{false};
    uint16_t explicitDataBlocks{0};
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
