#pragma once

#include <cstddef>
#include <cstdint>

#include "ConfigROMConstants.hpp"
#include "ROMScanNodeStateMachine.hpp"
#include "SpeedPolicy.hpp"
#include "../Async/AsyncTypes.hpp"

namespace ASFW::Discovery {

class GenerationContextPolicy {
public:
    [[nodiscard]] static constexpr bool IsCurrentEvent(Generation eventGeneration,
                                                       Generation activeGeneration) noexcept {
        return eventGeneration != 0 && eventGeneration == activeGeneration;
    }

    [[nodiscard]] static constexpr bool CanRestartIdleScan(Generation activeGeneration,
                                                           bool scannerIdle,
                                                           Generation requestedGeneration) noexcept {
        return scannerIdle && requestedGeneration != 0 && requestedGeneration != activeGeneration;
    }

    [[nodiscard]] static constexpr bool MatchesActiveScan(Generation requestedGeneration,
                                                          Generation activeGeneration) noexcept {
        return requestedGeneration == activeGeneration;
    }
};

class ShortReadResolutionPolicy {
public:
    [[nodiscard]] static constexpr bool IsValidQuadletPayload(size_t payloadSizeBytes) noexcept {
        return payloadSizeBytes == ASFW::ConfigROM::kQuadletBytes;
    }

    [[nodiscard]] static constexpr bool ShouldTreatAsEOF(Async::AsyncStatus status,
                                                         size_t payloadSizeBytes,
                                                         uint32_t completedQuadlets) noexcept {
        return completedQuadlets > 0 &&
               (status != Async::AsyncStatus::kSuccess || !IsValidQuadletPayload(payloadSizeBytes));
    }

    [[nodiscard]] static constexpr bool IsReadFailure(Async::AsyncStatus status,
                                                      size_t payloadSizeBytes,
                                                      uint32_t completedQuadlets) noexcept {
        return !ShouldTreatAsEOF(status, payloadSizeBytes, completedQuadlets) &&
               (status != Async::AsyncStatus::kSuccess || !IsValidQuadletPayload(payloadSizeBytes));
    }

    [[nodiscard]] static constexpr uint16_t ClampHeaderFirstEntryCount(uint16_t entryCount) noexcept {
        if (entryCount > ASFW::ConfigROM::kHeaderFirstMaxEntries) {
            return static_cast<uint16_t>(ASFW::ConfigROM::kHeaderFirstMaxEntries);
        }
        return entryCount;
    }
};

class RetryBackoffPolicy {
public:
    enum class Decision : uint8_t {
        RetrySameSpeed,
        RetryWithFallback,
        FailedExhausted,
    };

    template <typename TransitionFn>
    [[nodiscard]] Decision Apply(ROMScanNodeStateMachine& node,
                                 SpeedPolicy& speedPolicy,
                                 uint8_t perStepRetries,
                                 TransitionFn&& transitionNodeState) const {
        if (node.RetriesLeft() > 0) {
            node.DecrementRetries();
            transitionNodeState(node,
                                ROMScanNodeStateMachine::State::Idle,
                                "RetryWithFallback retry same speed");
            return Decision::RetrySameSpeed;
        }

        speedPolicy.RecordTimeout(node.NodeId(), node.CurrentSpeed());

        const FwSpeed newSpeed = speedPolicy.ForNode(node.NodeId()).localToNode;
        if (newSpeed == node.CurrentSpeed()) {
            transitionNodeState(node,
                                ROMScanNodeStateMachine::State::Failed,
                                "RetryWithFallback exhausted retries");
            return Decision::FailedExhausted;
        }

        node.SetCurrentSpeed(newSpeed);
        node.SetRetriesLeft(perStepRetries);
        transitionNodeState(node,
                            ROMScanNodeStateMachine::State::Idle,
                            "RetryWithFallback speed fallback");
        return Decision::RetryWithFallback;
    }
};

} // namespace ASFW::Discovery
