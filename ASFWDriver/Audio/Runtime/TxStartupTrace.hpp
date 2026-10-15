#pragma once

#include "../../Isoch/Core/IsochTxQueue.hpp"
#include "../../Common/TimingUtils.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>

namespace ASFW::Audio::Runtime {

inline constexpr int32_t kStartupSytPeriodTicks = 16 * ASFW::Timing::kTicksPerCycle;
inline constexpr int32_t kStartupSytHalfPeriodTicks = kStartupSytPeriodTicks / 2;

struct TxStartupClientSample final {
    uint64_t join{0};
    uint64_t sampleFrame{0};
    uint64_t hostTicks{0};
    uint64_t filledBefore{0};
    uint64_t exposedFrameEnd{0};
    uint64_t writtenFrameEnd{0};
    uint64_t anchorFrame{0};
    uint64_t anchorEpoch{0};
    uint32_t frames{0};
    uint32_t capacity{0};
};

// Work queue arms a join; IO publishes its first write without logging or
// allocating. The preparation queue drains immutable records. Never reuse a
// slot while the object lives, including across retained-wire joins.
struct TxStartupClientTrace final {
    static constexpr uint32_t kCapacity = 16;
    std::atomic<uint64_t> join{0};
    std::atomic<bool> pending{false};
    std::atomic<uint32_t> count{0};
    uint32_t readCursor{0};
    TxStartupClientSample samples[kCapacity]{};

    void ArmJoin() noexcept {
        join.fetch_add(1, std::memory_order_relaxed);
        pending.store(true, std::memory_order_release);
    }
    void Publish(TxStartupClientSample sample) noexcept {
        if (!pending.exchange(false, std::memory_order_acq_rel)) return;
        const uint32_t index = count.load(std::memory_order_relaxed);
        if (index >= kCapacity) return;
        sample.join = join.load(std::memory_order_acquire);
        samples[index] = sample;
        count.store(index + 1, std::memory_order_release);
    }
};

// IO publishes once per sampled packet; preparation reads immutable evidence.
// Reset only on a fresh wire arm after IO has quiesced, never on a rejoin.
struct TxStartupFillSample final {
    uint64_t packet{0}, frame{0}, payloadOffset{0}, firstWritable{0};
    uint32_t sourceBits[2]{}, encodedWords[2]{};
};
struct TxStartupFillTrace final {
    std::atomic<uint32_t> count{0};
    TxStartupFillSample samples[Isoch::kTxDiagnosticCapacity]{};
    void Publish(const TxStartupFillSample& sample) noexcept {
        const uint32_t n = count.load(std::memory_order_relaxed);
        if (n == Isoch::kTxDiagnosticCapacity || (n && samples[n - 1].packet == sample.packet)) return;
        samples[n] = sample;
        count.store(n + 1, std::memory_order_release);
    }
};

// Decode transport's immutable raw completion sample on the audio side.
// A prefix is content evidence, not a capture of the whole packet on the bus.
struct TxStartupPacket final {
    bool valid{false};
    uint8_t dbs{0};
    uint8_t dbc{0};
    uint8_t fdf{0};
    uint16_t syt{0xffff};
    uint32_t blocks{0};
    uint32_t peakQ24{0};
    int32_t sytLeadTicks{0};
    uint32_t isochHeader{0};
    uint32_t firstEventWords[2]{};
};

[[nodiscard]] inline TxStartupPacket DecodeTxStartupPacket(
    const Isoch::IsochTxDiagnosticPacket& sample, bool rawSigned24 = false,
    uint32_t pcmChannels = UINT32_MAX) noexcept {
    TxStartupPacket out{};
    if (!sample.metadataMatches || !sample.descriptorMatches ||
        sample.prefixLength < 8 || sample.prefixLength > sizeof(sample.prefix) ||
        sample.payloadLength < 8 || sample.prefixLength > sample.payloadLength) return out;
    const auto* p = sample.prefix;
    uint32_t lengthHeader = sample.immediateHeader[1];
    out.isochHeader = sample.immediateHeader[0];
    if constexpr (std::endian::native == std::endian::big) {
        lengthHeader = std::byteswap(lengthHeader);
        out.isochHeader = std::byteswap(out.isochHeader);
    }
    if ((lengthHeader >> 16) != sample.payloadLength) return out;
    out.dbs = p[1];
    out.dbc = p[3];
    out.fdf = p[5];
    out.syt = static_cast<uint16_t>((p[6] << 8) | p[7]);
    if (out.syt != 0xffff && (out.syt & 0xfffU) >= ASFW::Timing::kTicksPerCycle) return out;
    if ((p[4] & 0x3fU) != 0x10 || out.dbs == 0 ||
        (sample.payloadLength - 8) % (4U * out.dbs) != 0) return out;
    out.blocks = (sample.payloadLength - 8) / (4U * out.dbs);
    out.valid = true;
    for (uint32_t offset = 8; offset + 4 <= sample.prefixLength; offset += 4) {
        if (offset < 16) {
            out.firstEventWords[(offset - 8) / 4] = (uint32_t{p[offset]} << 24) |
                (uint32_t{p[offset + 1]} << 16) | (uint32_t{p[offset + 2]} << 8) | p[offset + 3];
        }
        // AM824 MBLA label; MIDI and other event types are not PCM.
        const uint32_t channel = ((offset - 8) / 4) % out.dbs;
        if (channel >= pcmChannels || (!rawSigned24 && p[offset] != 0x40)) continue;
        const uint32_t raw = (uint32_t{p[offset + 1]} << 16) |
            (uint32_t{p[offset + 2]} << 8) | p[offset + 3];
        const uint32_t magnitude = (raw & 0x800000U) ? 0x1000000U - raw : raw;
        out.peakQ24 = std::max(out.peakQ24, magnitude);
    }
    if (out.syt != 0xffff) {
        // SYT is modulo 16 cycles. OUTPUT_LAST timestamps omit the offset;
        // report lead relative to the start of the observed transmit cycle.
        const int32_t cycle = static_cast<int32_t>((sample.cycleTimestamp >> 12) & 15U);
        out.sytLeadTicks = static_cast<int32_t>((out.syt >> 12) * ASFW::Timing::kTicksPerCycle +
            (out.syt & 0xfff)) - cycle * static_cast<int32_t>(ASFW::Timing::kTicksPerCycle);
        if (out.sytLeadTicks < -kStartupSytHalfPeriodTicks) out.sytLeadTicks += kStartupSytPeriodTicks;
        if (out.sytLeadTicks >= kStartupSytHalfPeriodTicks) out.sytLeadTicks -= kStartupSytPeriodTicks;
    }
    return out;
}

struct TxStartupTrace final {
    uint32_t cursor{0};
    uint32_t matchedFills{0}, changedFills{0}, matchReports{0};
    uint32_t invalid{0};
    uint32_t mappingMismatch{0};
    uint32_t metadataMismatch{0};
    uint32_t data{0};
    uint32_t noData{0};
    uint32_t nonzeroPrefixes{0};
    uint32_t dbcDiscontinuities{0};
    int32_t minimumLead{kStartupSytHalfPeriodTicks};
    int32_t maximumLead{-kStartupSytHalfPeriodTicks};
    uint32_t timedPackets{0};
    uint32_t lastReportSecond{UINT32_MAX};
    uint64_t previousPacket{UINT64_MAX};
    TxStartupPacket previous{};

    void Observe(const Isoch::IsochTxDiagnosticPacket& raw,
                 const TxStartupPacket& packet) noexcept {
        metadataMismatch += !raw.metadataMatches;
        mappingMismatch += raw.metadataMatches && !raw.descriptorMatches;
        invalid += !packet.valid;
        if (packet.valid) {
            data += packet.blocks != 0;
            noData += packet.blocks == 0;
            nonzeroPrefixes += packet.peakQ24 != 0;
            if (previous.valid && previousPacket + 1 == raw.packetIndex &&
                packet.dbc != static_cast<uint8_t>(previous.dbc + previous.blocks))
                ++dbcDiscontinuities;
            if (packet.syt != 0xffff) {
                ++timedPackets;
                minimumLead = std::min(minimumLead, packet.sytLeadTicks);
                maximumLead = std::max(maximumLead, packet.sytLeadTicks);
            }
        }
        previousPacket = raw.packetIndex;
        previous = packet;
    }
};

} // namespace ASFW::Audio::Runtime
