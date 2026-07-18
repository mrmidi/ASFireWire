#pragma once

#include "AmdtpCadence.hpp"
#include "AmdtpPacketTimeline.hpp"
#include "AmdtpTypes.hpp"
#include "../IEC61883/CipHeader.hpp"
#include "../IEC61883/DbcCounter.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

namespace ASFW::Protocols::Audio::AMDTP {

// Lock-free, best-effort view for the CoreAudio callback / diagnostic drain.
// Packet preparation owns the mutable cursor; diagnostics must not read that
// state directly across the queue boundary.
struct AmdtpTxPacketizerTelemetrySnapshot final {
    uint64_t nextAudioFrame{0};
    uint64_t lastDataFirstAudioFrame{0};
    uint64_t lastDataEndAudioFrame{0};
    uint64_t lastDataPacketIndex{0};
    uint64_t cursorEpoch{0};
    bool frameCursorAligned{false};
    bool hasLastDataPacket{false};
};

class AmdtpTxPacketizer final {
public:
    AmdtpTxPacketizer() noexcept = default;

    bool Configure(const AmdtpStreamConfig& streamConfig,
                   const AmdtpTxPolicy& txPolicy) noexcept;

    void BindTimeline(AmdtpPacketTimeline* timeline) noexcept;

    void Reset(uint8_t initialDbc = 0,
               uint64_t initialAudioFrame = 0) noexcept;

    // Timing may reacquire during a running stream. Content-frame ownership
    // must not reacquire with it, so alignment is accepted only once after
    // Reset().
    [[nodiscard]] bool AlignFrameCursorOnce(uint64_t frameIndex) noexcept;

    // Re-arm the one-shot frame-cursor alignment WITHOUT resetting DBC/cadence.
    // Used when the RX replay the frame cursor rides went unavailable (epoch
    // reset / underrun during aggregate churn): the cursor would otherwise stay
    // frozen at its pre-stall frame while CoreAudio's write cursor advances,
    // and once it falls more than a playback ring behind, TX transmits the
    // overwritten (silent) region forever. Re-arming lets the first DATA packet
    // after replay recovers re-project the cursor to the live frame. Only fires
    // on genuine replay stalls, so a healthy (e.g. direct-bound) stream that
    // never stalls is unaffected.
    void ReArmFrameCursorAlignment() noexcept;

    bool PrepareNextPacket(TxPacketSlotView slot,
                           const AmdtpTimingState& timing,
                           PreparedTxPacket& outPacket) noexcept;

    [[nodiscard]] const AmdtpStreamConfig& StreamConfig() const noexcept;
    [[nodiscard]] const AmdtpTxPolicy& TxPolicy() const noexcept;
    [[nodiscard]] bool NextPacketWouldCarryData() const noexcept;

    [[nodiscard]] AmdtpTxPacketizerTelemetrySnapshot
    TelemetrySnapshot() const noexcept;

private:
    void PublishTelemetrySnapshot() noexcept;

    void WriteDataPacketDefaults(uint8_t* packetBytes,
                                 uint32_t packetCapacityBytes,
                                 uint32_t payloadBytes) noexcept;

    void WriteCipHeader(uint8_t* packetBytes,
                        const IEC61883::CipHeaderWords& header) noexcept;

    [[nodiscard]] uint32_t DataPacketBytes() const noexcept;
    [[nodiscard]] uint32_t PayloadBytes() const noexcept;

    AmdtpStreamConfig streamConfig_{};
    AmdtpTxPolicy txPolicy_{};

    IEC61883::CipHeaderBuilder cipBuilder_{};
    IEC61883::DbcCounter dbcCounter_{};

    Blocking48kCadence blocking48kCadence_{};
    NonBlocking48kCadence nonBlocking48kCadence_{};
    IAmdtpCadence* cadence_{nullptr};

    AmdtpPacketTimeline* timeline_{nullptr};

    uint64_t nextAudioFrame_{0};
    bool frameCursorAligned_{false};
    uint64_t cursorEpoch_{0};
    uint64_t lastDataFirstAudioFrame_{0};
    uint64_t lastDataEndAudioFrame_{0};
    uint64_t lastDataPacketIndex_{0};
    bool hasLastDataPacket_{false};

    std::atomic<uint64_t> telemetryNextAudioFrame_{0};
    std::atomic<uint64_t> telemetryLastDataFirstAudioFrame_{0};
    std::atomic<uint64_t> telemetryLastDataEndAudioFrame_{0};
    std::atomic<uint64_t> telemetryLastDataPacketIndex_{0};
    std::atomic<uint64_t> telemetryCursorEpoch_{0};
    std::atomic<bool> telemetryFrameCursorAligned_{false};
    std::atomic<bool> telemetryHasLastDataPacket_{false};
};

} // namespace ASFW::Protocols::Audio::AMDTP
