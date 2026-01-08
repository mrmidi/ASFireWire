// SimITEngine.hpp
// ASFW - Isochronous Transmit Simulation Engine
//
// Hardware-grade offline testing harness that enforces the same invariants
// as real FireWire IT hardware:
//   - Fixed 8 kHz cadence (8 packets per 1ms tick)
//   - Bounded latency detection
//   - Deterministic refill rules
//   - Ruthless monitoring
//
// Usage:
//   1. Configure() with SimITConfig
//   2. Start(nowNs) to begin simulation
//   3. WritePCMInterleavedS32() from producer (e.g., CoreAudio callback)
//   4. Tick1ms(nowNs) from 1 kHz watchdog - always emits 8 packets
//   5. Check AnomaliesCount() for violations
//
// Reference: docs/Isoch/ISOCH_MONITORING.md
//

#pragma once

#include <array>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "../Encoding/PacketAssembler.hpp"

namespace ASFW::Isoch::Sim {

namespace ASFW::Isoch::Sim {

struct SimITConfig final {
    uint32_t packetsPerTick            = 8;
    uint32_t cycleGroupSize            = 8;
    uint8_t  dataCycleMask             = 0xEE;
    uint8_t  dataBlocksPerDataPacket   = 8;
    uint32_t dataPacketSizeBytes       = 72;
    uint32_t noDataPacketSizeBytes     = 8;
    uint16_t sytValue                  = 0xFFFF;

    uint64_t expectedTickIntervalNs    = 1'000'000;
    uint64_t lateTickThresholdNs       = 2'000'000;
};

enum class SimState : uint8_t { Stopped, Running };

template <typename PacketT>
struct PacketAccess {
    static bool     IsData(const PacketT& p) noexcept { return static_cast<bool>(p.isData); }
    static uint8_t  Dbc(const PacketT& p) noexcept    { return static_cast<uint8_t>(p.dbc); }
    static uint32_t Size(const PacketT& p) noexcept   { return static_cast<uint32_t>(p.size); }
};

enum class AnomalyKind : uint8_t {
    CadenceMismatch,
    SizeMismatch,
    DbcMismatch,
    LateTick
};

struct Anomaly final {
    AnomalyKind kind{};
    uint64_t seq{0};

    uint64_t tickIndex : 48;
    uint64_t cycleInGroup : 8;
    uint64_t reserved : 8;

    uint32_t expectedSize{0};
    uint32_t actualSize{0};
    uint8_t  expectedDbc{0};
    uint8_t  actualDbc{0};
    uint8_t  expectedIsData{0};
    uint8_t  actualIsData{0};
    uint32_t ringFill{0};
};

class SimITEngine final {
public:
    SimITEngine() noexcept = default;

    void Configure(const SimITConfig& cfg, uint8_t sid, uint8_t initialDbc) noexcept {
        cfg_ = cfg;
        assembler_.setSID(sid);
        assembler_.reset(initialDbc);

        expectedDbcForNextData_ = initialDbc;
        cycleInGroup_ = 0;

        state_ = SimState::Stopped;
        tickIndex_ = 0;
        lastTickNs_ = 0;

        packetsTotal_ = 0;
        packetsData_ = 0;
        packetsNoData_ = 0;

        producerOverruns_ = 0;
        lateTickCount_ = 0;

        anomaliesSeq_ = 0;
        anomaliesWrite_ = 0;
        anomaliesCount_ = 0;

        lastAssemblerUnderrunCount_ = assembler_.underrunCount();
        underrunPacketsSynthesized_ = 0;
    }

    void Start(uint64_t nowNs) noexcept {
        lastTickNs_ = nowNs;
        tickIndex_ = 0;
        state_ = SimState::Running;

        lastAssemblerUnderrunCount_ = assembler_.underrunCount();
        underrunPacketsSynthesized_ = 0;
    }

    void Stop() noexcept { state_ = SimState::Stopped; }

    SimState State() const noexcept { return state_; }

    ASFW::Encoding::StereoAudioRingBuffer& RingBuffer() noexcept { return assembler_.ringBuffer(); }

    uint32_t WritePCMInterleavedS32(const int32_t* interleavedStereoS32, uint32_t frames) noexcept {
        const uint32_t written = assembler_.ringBuffer().write(interleavedStereoS32, frames);
        if (written < frames) {
            producerOverruns_++;
        }
        return written;
    }

    void Tick1ms(uint64_t nowNs) noexcept {
        if (state_ != SimState::Running) {
            return;
        }

        const uint64_t dt = (lastTickNs_ == 0) ? cfg_.expectedTickIntervalNs : (nowNs - lastTickNs_);
        if (dt > cfg_.lateTickThresholdNs) {
            lateTickCount_++;
            PushAnomalyLateTick(dt);
        }
        lastTickNs_ = nowNs;

        for (uint32_t i = 0; i < cfg_.packetsPerTick; ++i) {
            const bool expectedIsData = ((cfg_.dataCycleMask >> cycleInGroup_) & 0x1u) != 0;

            auto pkt = assembler_.assembleNext(cfg_.sytValue);

            packetsTotal_++;
            const bool actualIsData = PacketAccess<decltype(pkt)>::IsData(pkt);
            if (actualIsData) {
                packetsData_++;
            } else {
                packetsNoData_++;
            }

            ValidatePacket(pkt, expectedIsData);

            cycleInGroup_++;
            if (cycleInGroup_ >= cfg_.cycleGroupSize) {
                cycleInGroup_ = 0;
            }
        }

        const uint64_t u = assembler_.underrunCount();
        if (u > lastAssemblerUnderrunCount_) {
            underrunPacketsSynthesized_ += (u - lastAssemblerUnderrunCount_);
            lastAssemblerUnderrunCount_ = u;
        }

        tickIndex_++;
    }

    uint64_t PacketsTotal() const noexcept { return packetsTotal_; }
    uint64_t PacketsData() const noexcept { return packetsData_; }
    uint64_t PacketsNoData() const noexcept { return packetsNoData_; }

    uint64_t ProducerOverruns() const noexcept { return producerOverruns_; }
    uint64_t LateTickCount() const noexcept { return lateTickCount_; }

    uint32_t RingFillLevelFrames() const noexcept { return assembler_.bufferFillLevel(); }

    uint64_t AssemblerUnderrunCount() const noexcept { return assembler_.underrunCount(); }

    uint64_t UnderrunPacketsSynthesized() const noexcept { return underrunPacketsSynthesized_; }

    static constexpr uint32_t kAnomalyCapacity = 256;

    uint32_t AnomaliesCount() const noexcept { return anomaliesCount_; }

    uint32_t CopyAnomalies(Anomaly* out, uint32_t max) const noexcept {
        if (!out || max == 0) return 0;

        const uint32_t count = (anomaliesCount_ < kAnomalyCapacity) ? anomaliesCount_ : kAnomalyCapacity;
        const uint32_t n = (max < count) ? max : count;

        const uint32_t start = (anomaliesCount_ <= kAnomalyCapacity)
            ? 0u
            : (anomaliesWrite_ & (kAnomalyCapacity - 1u));

        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t idx = (start + i) & (kAnomalyCapacity - 1u);
            out[i] = anomalies_[idx];
        }
        return n;
    }

private:
    void ValidatePacket(const auto& pkt, bool expectedIsData) noexcept {
        const bool actualIsData = PacketAccess<std::decay_t<decltype(pkt)>>::IsData(pkt);
        const uint8_t actualDbc = PacketAccess<std::decay_t<decltype(pkt)>>::Dbc(pkt);
        const uint32_t actualSize = PacketAccess<std::decay_t<decltype(pkt)>>::Size(pkt);

        if (actualIsData != expectedIsData) {
            PushAnomaly(AnomalyKind::CadenceMismatch,
                        expectedIsData, actualIsData,
                        ExpectedSizeBytes(expectedIsData), actualSize,
                        expectedDbcForNextData_, actualDbc);
        }

        const uint32_t expectedSize = ExpectedSizeBytes(expectedIsData);
        if (actualSize != expectedSize) {
            PushAnomaly(AnomalyKind::SizeMismatch,
                        expectedIsData, actualIsData,
                        expectedSize, actualSize,
                        expectedDbcForNextData_, actualDbc);
        }

        const uint8_t expectedDbc = expectedDbcForNextData_;
        if (actualDbc != expectedDbc) {
            PushAnomaly(AnomalyKind::DbcMismatch,
                        expectedIsData, actualIsData,
                        expectedSize, actualSize,
                        expectedDbc, actualDbc);
        }

        if (expectedIsData) {
            expectedDbcForNextData_ = static_cast<uint8_t>(expectedDbcForNextData_ + cfg_.dataBlocksPerDataPacket);
        }
    }

    uint32_t ExpectedSizeBytes(bool expectedIsData) const noexcept {
        return expectedIsData ? cfg_.dataPacketSizeBytes : cfg_.noDataPacketSizeBytes;
    }

    void PushAnomalyLateTick(uint64_t dtNs) noexcept {
        const uint32_t ringFill = assembler_.bufferFillLevel();
        Anomaly a{};
        a.kind = AnomalyKind::LateTick;
        a.seq = ++anomaliesSeq_;
        a.tickIndex = tickIndex_;
        a.cycleInGroup = cycleInGroup_;
        a.expectedSize = static_cast<uint32_t>(cfg_.expectedTickIntervalNs);
        a.actualSize = static_cast<uint32_t>((dtNs > 0xFFFFFFFFu) ? 0xFFFFFFFFu : dtNs);
        a.expectedDbc = expectedDbcForNextData_;
        a.actualDbc = expectedDbcForNextData_;
        a.expectedIsData = 0;
        a.actualIsData = 0;
        a.ringFill = ringFill;
        StoreAnomaly(a);
    }

    void PushAnomaly(AnomalyKind kind,
                     bool expectedIsData, bool actualIsData,
                     uint32_t expectedSize, uint32_t actualSize,
                     uint8_t expectedDbc, uint8_t actualDbc) noexcept
    {
        Anomaly a{};
        a.kind = kind;
        a.seq = ++anomaliesSeq_;
        a.tickIndex = tickIndex_;
        a.cycleInGroup = cycleInGroup_;
        a.expectedSize = expectedSize;
        a.actualSize = actualSize;
        a.expectedDbc = expectedDbc;
        a.actualDbc = actualDbc;
        a.expectedIsData = static_cast<uint8_t>(expectedIsData ? 1 : 0);
        a.actualIsData = static_cast<uint8_t>(actualIsData ? 1 : 0);
        a.ringFill = assembler_.bufferFillLevel();
        StoreAnomaly(a);
    }

    void StoreAnomaly(const Anomaly& a) noexcept {
        const uint32_t idx = anomaliesWrite_ & (kAnomalyCapacity - 1u);
        anomalies_[idx] = a;
        anomaliesWrite_++;
        if (anomaliesCount_ < kAnomalyCapacity) {
            anomaliesCount_++;
        } else {
            anomaliesCount_ = kAnomalyCapacity;
        }
    }

private:
    SimITConfig cfg_{};
    SimState state_{SimState::Stopped};

    ASFW::Encoding::PacketAssembler assembler_;

    uint8_t expectedDbcForNextData_{0};
    uint32_t cycleInGroup_{0};

    uint64_t tickIndex_{0};
    uint64_t lastTickNs_{0};

    uint64_t packetsTotal_{0};
    uint64_t packetsData_{0};
    uint64_t packetsNoData_{0};

    uint64_t producerOverruns_{0};
    uint64_t lateTickCount_{0};

    uint64_t lastAssemblerUnderrunCount_{0};
    uint64_t underrunPacketsSynthesized_{0};

    std::array<Anomaly, kAnomalyCapacity> anomalies_{};
    uint64_t anomaliesSeq_{0};
    uint32_t anomaliesWrite_{0};
    uint32_t anomaliesCount_{0};
};

} // namespace ASFW::Isoch::Sim

} // namespace ASFW::Isoch::Sim
