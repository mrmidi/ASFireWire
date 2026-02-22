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

namespace ASFW::Isoch::Rx {

class IsochAudioRxPipeline final {
public:
    void ConfigureFor48k() noexcept;

    void OnStart() noexcept;
    void OnStop() noexcept;

    void OnPacket(const uint8_t* payload, size_t length) noexcept;

    void OnPollEnd(Driver::HardwareInterface& hw,
                   uint32_t packetsProcessed,
                   uint64_t pollStartMachTicks) noexcept;

    void SetSharedRxQueue(void* base, uint64_t bytes) noexcept;
    void SetExternalSyncBridge(Core::ExternalSyncBridge* bridge) noexcept;

    [[nodiscard]] StreamProcessor& StreamProcessorRef() noexcept { return streamProcessor_; }

private:
    static constexpr uint64_t kExternalSyncStaleNanos = 100'000'000ULL; // 100ms

    StreamProcessor streamProcessor_{};
    Shared::TxSharedQueueSPSC rxSharedQueue_{};

    Core::ExternalSyncBridge* externalSyncBridge_{nullptr};
    Core::ExternalSyncClockState externalSyncClockState_{};

    struct CycleTimeCorrelation {
        uint32_t prevCycleTimer{0};
        uint64_t prevHostTicks{0};
        bool     hasPrevious{false};
        uint32_t pollsSinceLastUpdate{0};
        double   sampleRate{48000.0};
    } cycleCorr_{};
};

} // namespace ASFW::Isoch::Rx
