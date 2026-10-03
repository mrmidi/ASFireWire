#pragma once

#include "PcmSlotMap.hpp"

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
    /// Signed 24-bit sample in bits 31:8 of a little-endian quadlet.
    RawPcm24Upper24In32LE = 3,
};

enum class DbsPolicy : uint8_t {
    Constant = 0,
    VariablePerPacket = 1,
};

enum class TxPayloadLayout : uint8_t {
    QuadletSlots = 0,
    MotuV3Packed = 1,
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
    /// CIP SPH bit (Q0 bit 10); see Isoch::Audio::AudioStreamConfig::cipSph.
    bool cipSph{false};
    /// Keep `fdf` instead of deriving the AM824 SFC from the rate; see
    /// Isoch::Audio::AudioStreamConfig::fdfIsFixed.
    bool fdfIsFixed{false};

    uint8_t framesPerDataPacket{8};
    uint32_t maxPacketBytes{512};

    // First host buffer channel this stream encodes. For a multi-stream device
    // (Venice F32 = 2×16) the 32-ch host output buffer is split across streams:
    // stream 0 reads channels [0, pcmChannels), stream 1 reads [16, 16+pcmChannels),
    // etc. Single-stream devices keep 0.
    uint8_t sourceChannelOffset{0};

    /// Content framing. Defaults to the existing 8-byte IEC 61883 CIP header.
    enum class PacketFraming : uint8_t { Cip = 0, Headerless = 1 };
    PacketFraming packetFraming{PacketFraming::Cip};
    uint8_t isochTag{1};
    uint8_t isochSync{0};
};

struct AmdtpTxPolicy final {
    PcmSlotEncoding hostToDevicePcmEncoding{PcmSlotEncoding::Am824MBLA};
    TxPayloadLayout payloadLayout{TxPayloadLayout::QuadletSlots};
    DbsPolicy dbsPolicy{DbsPolicy::Constant};

    uint32_t defaultNonAudioSlotWord{0x80000000};
    bool clearPayloadBeforeExposure{true};
    bool initializeNonAudioSlots{true};
    bool preserveFdfInNoDataPackets{false};
    bool emptyPacketsDuringIdle{false};
    /// Some device families require a full blocking packet at cadence NO-DATA
    /// phases. Those packets carry the scheduled blocks but use this audio-slot
    /// label instead of PCM; non-audio slots retain defaultNonAudioSlotWord.
    bool cadencePacketsCarryDataBlocks{false};
    uint32_t cadenceSlotWord{0xCF000000};
    /// Write the DBC of the block *after* this packet's last one rather than of its
    /// first, i.e. advance before writing. IEC 61883-1 counts from the first block; MOTU
    /// devices count the end, and Linux sets CIP_DBC_IS_END_EVENT on every MOTU transmit
    /// stream for it (amdtp-motu.c:465, applied at amdtp-stream.c:1040-1046).
    bool dbcIsEndEvent{false};

    /// Logical host PCM channel -> AM824 slot mapping.
    ASFW::Audio::Wire::PcmSlotMap playbackChannelMap{};
};

struct HostAudioBufferView final {
    const float* interleavedFloat32{nullptr};

    uint64_t firstFrame{0};
    uint32_t frameCount{0};
    uint32_t frameCapacity{0};
    uint32_t channels{0};
};

struct TxPacketSlotView final {
    uint64_t packetIndex{0};
    uint8_t* bytes{nullptr};
    uint32_t capacityBytes{0};
};

struct PreparedTxPacket final {
    uint64_t packetIndex{0};
    uint32_t byteCount{0};

    bool isData{false};
    enum class Operation : uint8_t { Packet = 0, SkipCycle = 1 };
    Operation operation{Operation::Packet};
    uint8_t isochTag{1};
    uint8_t isochSync{0};
    uint8_t dbc{0};
    uint16_t syt{0xFFFF};
    uint32_t firstMotuSph{0};
    bool hasMotuSph{false};

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
    /// Bus cycle (0..7999) this packet is transmitted in. MOTU bases each block's SPH on
    /// it (write_sph, amdtp-motu.c:373-393); families that time by SYT ignore it.
    uint32_t transmitCycle{0};
    bool transmitCycleValid{false};
    /// The same transmit time in full, on the cycle-timer offset domain and with the
    /// anchoring completion stamp's sub-cycle phase; valid with transmitCycleValid.
    /// MOTU V3 seeds its free-running SPH clock from it (MotuV3TxTimingStamper).
    int64_t transmitTicks{0};
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
    // MOTU protocol-v2: 3-byte PCM chunks from byte offset 10 of a data block, behind an
    // SPH quadlet and two message chunks. Not a quadlet-slot format, so the slot-based
    // encode/decode helpers do not apply -- see Audio/Wire/MOTU.
    kMotuV2 = 2,
    /// Headerless RME-style signed 24-in-32 with significant bits at [31:8].
    kRawPcm24Upper24In32LE = 3,
    // MOTU protocol-v3 (828 Mk3): the same block layout as kMotuV2 behind a
    // CIP header whose EOH1 bit is clear. Deliberately not 2: that value is
    // kMotuV2, and sharing it would route V3 through every V2 branch.
    kMotuV3Packed = 4,
};

enum class AudioPacketFraming : uint8_t {
    kCip = 0,
    kHeaderless = 1,
    /// MOTU protocol-v3 capture: an eight-byte header in the CIP position whose
    /// EOH1 bit is clear, so the IEC 61883 decoder rejects it. It carries no
    /// usable DBS, DBC or SYT; the block stride comes from the payload codec.
    kMotuV3Header = 2,
};

} // namespace ASFW::Encoding
