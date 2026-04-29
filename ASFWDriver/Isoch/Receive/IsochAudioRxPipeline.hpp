// IsochAudioRxPipeline.hpp
// ASFW - Audio RX pipeline (CIP/AM824, shared queue pump, external sync, clock correlation).

#pragma once

#include "StreamProcessor.hpp"
#include "../Core/ExternalSyncBridge.hpp"
#include "../Encoding/TimingUtils.hpp"

#include "../../Hardware/HardwareInterface.hpp"
#include "../../Diagnostics/Signposts.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Shared/TxSharedQueue.hpp"

#include <atomic>
#include <cstdint>
#include <functional>

namespace ASFW::Isoch::Rx {

class IsochAudioRxPipeline final {
public:
    using TimingLossCallback = std::function<void()>;

    struct ClockHealthSnapshot {
        bool active{false};
        bool clockEstablished{false};
        bool startupQualified{false};
        uint32_t updateSeq{0};
        uint16_t lastSyt{Core::ExternalSyncBridge::kNoInfoSyt};
        uint8_t lastFdf{0};
        uint8_t lastDbs{0};
        uint64_t lostCount{0};
    };

    void ConfigureFor48k() noexcept;

    void OnStart() noexcept;
    void OnStop() noexcept;

    void OnPacket(const uint8_t* payload, size_t length) noexcept;

    void OnPollEnd(Driver::HardwareInterface& hw,
                   uint32_t packetsProcessed,
                   uint64_t pollStartMachTicks) noexcept;

    void SetSharedRxQueue(uint8_t* base, uint64_t bytes) noexcept;
    void SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept;
    void SetTimingLossCallback(TimingLossCallback callback) noexcept;

    [[nodiscard]] StreamProcessor& StreamProcessorRef() noexcept { return streamProcessor_; }
    [[nodiscard]] ClockHealthSnapshot ReadClockHealth() const noexcept;

private:
    static constexpr uint64_t kExternalSyncStaleNanos = 100'000'000ULL; // 100ms

    void PublishTransportTimingAnchor(uint64_t hostTicks, bool fromCycleCorrelation) noexcept;

    StreamProcessor streamProcessor_{};
    Shared::TxSharedQueueSPSC rxSharedQueue_{};

    Core::ExternalSyncBridge* externalSyncBridge_{nullptr};
    Core::ExternalSyncClockState externalSyncClockState_{};
    TimingLossCallback timingLossCallback_{};

    struct CycleTimeCorrelation {
        uint32_t prevCycleTimer{0};
        uint64_t prevHostTicks{0};
        bool     hasPrevious{false};
        uint32_t pollsSinceLastUpdate{0};
        double   sampleRate{48000.0};
    } cycleCorr_{};

    uint64_t transportSampleFrame_{0};
    uint32_t transportHostNanosPerSampleQ8_{0};
    uint32_t transportTimingPublishCount_{0};
    uint32_t rxHealthPollCounter_{0};
    std::atomic<uint64_t> sytClockLostCount_{0};
};

} // namespace ASFW::Isoch::Rx
