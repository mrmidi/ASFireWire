#pragma once

#include "RxSytCadence.hpp"

#include <cstdint>

namespace ASFW::Driver {

class TxTimingModel final {
public:
    struct Config final {
        int64_t initialLeadTicks{3072}; // Saffire FillFirewireBuffers 0xec30
        uint32_t sytIntervalFrames{8};
        int32_t phaseDeadband{409};      // AllocateStreams 0x11f01
        int64_t tightLeadTicks{3072};   // below: near-underrun warning
        int64_t acceptLeadTicks{7620};  // at/above: device rejects (gate)
        int64_t escalateLeadTicks{12287}; // advisory log-escalation only
        // IEC 61883-6 TRANSFER_DELAY, added to the wire SYT at encode time
        // only. The lead fields above keep governing the raw fill phase
        // against the execution anchor (the Saffire FillFirewireBuffers
        // band); the receiver-facing presentation time is that phase plus
        // this delay. Default = Linux amdtp-stream.c parity for 48 kHz
        // blocking: (0x2E00 spec default - one cycle, which the SYT cycle
        // arithmetic contributes) + SYT_INTERVAL accumulation time
        // (TICKS_PER_SECOND * sytInterval / rate). See
        // TRANSFER_DELAY_AND_OTHER.md section 2.
        int64_t xmitTransferDelayTicks{12800};
    };

    enum class LeadHealth : uint8_t {
        kNotSeeded = 0,
        kAccepted,     // tight < lead < accept: the safe operating range
        kTightWarn,    // 0 <= lead <= tight: shipping while thin
        kLate,         // raw lead < 0: shipped, with wire lead diagnosed
        kGate,         // accept <= lead < escalate: device would reject
        kEscalate,     // lead >= escalate: drifted very far (advisory)
    };

    struct Decision final {
        uint16_t syt{0xFFFF};
        int64_t leadTicks{0};
        int64_t wireLeadTicks{0};
        LeadHealth health{LeadHealth::kNotSeeded};
        bool seededThisCall{false};

        // Resync diagnostics (for [TxSyt] telemetry / offline replay). Filled by
        // PeekNextDataSyt + AdjustOutputPhase so the caller can record the full
        // servo operand set without reaching into TxTimingModel internals.
        int64_t packetAnchorTicks{0};   // OHCI execution cycle for this packet
        int64_t phaseTicksPre{0};       // carried phase before AdjustOutputPhase
        int64_t phaseTicksPost{0};      // carried phase after AdjustOutputPhase
        int64_t recoveredPhaseTicks{0}; // rx.recoveredPhaseTicks (raw)
        int64_t rxPhaseDelayFree{0};    // servo target: recovered - transferDelay
        int64_t phaseError{0};
        int64_t frameError{0};
        int64_t correctionTicks{0};
        uint32_t rollingCadenceTicks{0};
        uint16_t pendingCadenceTicks{0}; // per-entry SYT delta the next commit applies
        uint16_t cadenceReadIndex{0};
        bool forceAdjustFired{false};   // correction path taken (not deadbanded)
        bool reseeded{false};           // gate/escalate un-seeded the model this call
    };

    static constexpr int64_t kTicksPerCycle = 3072;
    static constexpr int64_t kTicksPerFrame48k = 512;
    static constexpr int64_t kPacketStepTicks = 4096;  // 8 frames x 512
    static constexpr int64_t kSytDomainTicks = 49152;  // 16 cycles
    static constexpr int64_t kTicksPerSecond = 24576000;

    // IEC 61883-6 TRANSFER_DELAY for blocking transmission, in bus ticks.
    // 8704 = spec DEFAULT_TRANSFER_DELAY 0x2E00 (479.17 us) minus one cycle
    // (Linux amdtp-stream.c:303 keeps the same split because compute_syt's
    // cycle arithmetic supplies the +1-cycle term); the second term is the
    // SYT_INTERVAL event-accumulation time the blocking transmitter spends
    // filling a packet (amdtp-stream.c:307).
    [[nodiscard]] static constexpr int64_t XmitTransferDelayTicksForRate(
        uint32_t sytIntervalFrames, uint32_t sampleRateHz) noexcept {
        constexpr int64_t kDelayLessCycleTicks = 0x2E00 - kTicksPerCycle;
        if (sampleRateHz == 0) {
            return kDelayLessCycleTicks;
        }
        return kDelayLessCycleTicks +
               ((kTicksPerSecond * static_cast<int64_t>(sytIntervalFrames)) /
                static_cast<int64_t>(sampleRateHz));
    }

    TxTimingModel() noexcept = default;

    void Configure(const Config& config) noexcept;
    [[nodiscard]] const Config& GetConfig() const noexcept;

    // Un-seeds and re-arms the transmit anchor (call on stream start).
    void Reset() noexcept;

    // A cadence DATA opportunity could not be emitted after lock. Continuing
    // the carried phase would leave every later SYT one event behind, so the
    // next valid execution anchor must perform a fresh coarse acquisition.
    void RearmAfterSkippedDataSlot() noexcept;

    [[nodiscard]] bool IsSeeded() const noexcept;

    // The SYT the NEXT data packet would carry. `packetAnchorTicks` is the
    // projected OHCI execution cycle for that packet in the 8-second domain.
    // Before RX cadence lock, or outside Saffire's accepted lead window, the
    // decision remains unseeded and the caller emits SYT=0xffff.
    [[nodiscard]] Decision PeekNextDataSyt(
        int64_t packetAnchorTicks,
        const RxSytCadence& rxCadence) noexcept;

    // The packetizer actually emitted a data packet: advance one step.
    void CommitDataPacket() noexcept;

    [[nodiscard]] int64_t OutputPhaseTicks() const noexcept;

private:
    [[nodiscard]] int64_t AdjustOutputPhase(
        int64_t executionPhaseTicks,
        int64_t candidatePhaseTicks,
        const RxSytCadence::Snapshot& rx,
        Decision& decision) noexcept;
    [[nodiscard]] uint16_t SytForPhase(int64_t phaseTicks) const noexcept;
    [[nodiscard]] LeadHealth HealthForLead(int64_t leadTicks) const noexcept;

    Config config_{};
    int64_t phaseTicks_{0};
    bool seeded_{false};
    bool forceAdjust_{true};
    uint32_t cadenceEpoch_{0};
    uint16_t cadenceReadIndex_{RxSytCadence::kNoInfo};
    uint16_t pendingCadenceTicks_{0};
};

// The blocking accumulation term is a clean 4096 ticks at the power-of-two
// rates (SYT_INTERVAL doubles with the rate family), so the Config default
// must equal the ladder for the 48 kHz bring-up configuration.
static_assert(TxTimingModel::XmitTransferDelayTicksForRate(8, 48000) == 12800,
              "48 kHz blocking transfer delay must match the Config default");
static_assert(TxTimingModel::XmitTransferDelayTicksForRate(16, 96000) == 12800,
              "96 kHz blocking transfer delay (SYT_INTERVAL 16)");
static_assert(TxTimingModel::XmitTransferDelayTicksForRate(32, 192000) == 12800,
              "192 kHz blocking transfer delay (SYT_INTERVAL 32)");
static_assert(TxTimingModel::Config{}.xmitTransferDelayTicks ==
                  TxTimingModel::XmitTransferDelayTicksForRate(8, 48000),
              "Config default and rate ladder must agree");

} // namespace ASFW::Driver
