#pragma once

#include "../../../DriverKit/Config/DICE/DiceDeviceProfile.hpp"
#include "../../../DriverKit/Config/DICE/Isoch/DiceStreamConfig.hpp"
#include "../../../Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "../../../Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "../../../Ports/IAmdtpTxSlotProvider.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

struct DiceTxEngineCounters final {
    std::atomic<uint64_t> packetsPrepared{0};
    std::atomic<uint64_t> dataPacketsPrepared{0};
    std::atomic<uint64_t> noDataPacketsPrepared{0};
    std::atomic<uint64_t> slotAcquireFailures{0};
};

class DiceStreamConfigMapper final {
public:
    [[nodiscard]] static AMDTP::AmdtpStreamConfig ToAmdtpConfig(
        const ASFW::Isoch::Audio::DICE::DiceStreamConfig& diceConfig) noexcept;
};

class DiceTxStreamEngine final {
public:
    DiceTxStreamEngine() noexcept = default;

    bool Configure(const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile& profile,
                   const ASFW::Isoch::Audio::DICE::DiceStreamConfig& txConfig) noexcept;

    void BindSlotProvider(AMDTP::IAmdtpTxSlotProvider* slotProvider) noexcept;

    void ResetForStart(uint8_t initialDbc,
                       uint64_t initialAudioFrame) noexcept;

    bool PrepareNextTransmitSlot(uint32_t packetIndex,
                                 const AMDTP::AmdtpTimingState& timing) noexcept;
    [[nodiscard]] bool NextPacketWouldCarryData() const noexcept;

    void WriteHostOutputInt32(const AMDTP::HostAudioBufferView& hostBuffer) noexcept;

    [[nodiscard]] AMDTP::AmdtpPacketTimeline& Timeline() noexcept;
    [[nodiscard]] const AMDTP::AmdtpPacketTimeline& Timeline() const noexcept;

    [[nodiscard]] const DiceTxEngineCounters& Counters() const noexcept;

    [[nodiscard]] const AMDTP::AmdtpPayloadWriterCounters&
    PayloadWriterCounters() const noexcept;

private:
    AMDTP::AmdtpTxPolicy BuildTxPolicy(const ASFW::Isoch::Audio::DICE::DiceDeviceQuirks& quirks) const noexcept;

    const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile* profile_{nullptr};

    ASFW::Isoch::Audio::DICE::DiceStreamConfig diceConfig_{};
    ASFW::Isoch::Audio::DICE::DiceDeviceQuirks quirks_{};

    AMDTP::AmdtpTxPacketizer packetizer_{};
    AMDTP::AmdtpPayloadWriter payloadWriter_{};

    AMDTP::PacketTimelineSlot timelineSlots_[512]{};
    AMDTP::AmdtpPacketTimeline timeline_{};

    AMDTP::IAmdtpTxSlotProvider* slotProvider_{nullptr};

    DiceTxEngineCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::DICE
