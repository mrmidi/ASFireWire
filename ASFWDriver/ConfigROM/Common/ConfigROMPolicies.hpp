#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "../../Async/AsyncTypes.hpp"
#include "../../Discovery/SpeedPolicy.hpp"
#include "../Remote/ROMScanNodeStateMachine.hpp"
#include "ConfigROMConstants.hpp"

namespace ASFW::Discovery {

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

    [[nodiscard]] static constexpr uint16_t
    ClampHeaderFirstEntryCount(uint16_t entryCount) noexcept {
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
    [[nodiscard]] Decision Apply(ROMScanNodeStateMachine& node, SpeedPolicy& speedPolicy,
                                 uint8_t perStepRetries, TransitionFn&& transitionNodeState) const {
        if (node.RetriesLeft() > 0) {
            node.DecrementRetries();
            transitionNodeState(node, ROMScanNodeStateMachine::State::Idle,
                                "RetryWithFallback retry same speed");
            return Decision::RetrySameSpeed;
        }

        speedPolicy.RecordTimeout(node.NodeId(), node.CurrentSpeed());

        const FwSpeed newSpeed = speedPolicy.ForNode(node.NodeId()).localToNode;
        if (newSpeed == node.CurrentSpeed()) {
            transitionNodeState(node, ROMScanNodeStateMachine::State::Failed,
                                "RetryWithFallback exhausted retries");
            return Decision::FailedExhausted;
        }

        node.SetCurrentSpeed(newSpeed);
        node.SetRetriesLeft(perStepRetries);
        transitionNodeState(node, ROMScanNodeStateMachine::State::Idle,
                            "RetryWithFallback speed fallback");
        return Decision::RetryWithFallback;
    }
};

// After a bus reset Apple reads only a node's bus info block and keeps the ROM
// it cached for that device when the block is unchanged
// (IOFireWireROMCache::hasROMChanged, IOFireWireROMCache.cpp:240-320; resume
// in IOFireWireDevice.cpp:727-743). The block carries the GUID, the CRC of the
// ROM in its header quadlet, and the ROM generation a device advances when its
// ROM content changes (bus options [7:4]).
class UnchangedROMPolicy {
  public:
    // The previous generation's copy of the ROM whose bus info block equals
    // `busInfoBlockBE` (big-endian, header quadlet included), or nullptr when
    // the node has to be read in full.
    [[nodiscard]] static const ConfigROM* Find(std::span<const uint32_t> busInfoBlockBE,
                                               std::span<const ConfigROM> previous) noexcept {
        const auto it = std::ranges::find_if(
            previous, [busInfoBlockBE](const ConfigROM& rom) { return IsSame(busInfoBlockBE, rom); });
        return it != previous.end() ? std::to_address(it) : nullptr;
    }

    [[nodiscard]] static bool IsSame(std::span<const uint32_t> busInfoBlockBE,
                                     const ConfigROM& rom) noexcept {
        // A copy built around a failed read would otherwise be kept for good.
        if (rom.readIncomplete) {
            return false;
        }
        // The copy must hold more than the block: the root directory onward.
        if (busInfoBlockBE.empty() || rom.rawQuadlets.size() <= busInfoBlockBE.size()) {
            return false;
        }
        if (!std::ranges::equal(busInfoBlockBE,
                                std::span{rom.rawQuadlets}.first(busInfoBlockBE.size()))) {
            return false;
        }
        // Apple always re-reads a ROM of generation 0 that has no units: some
        // devices publish their units late without advancing the generation.
        return rom.bib.generation != 0 || !rom.unitDirectories.empty();
    }
};

} // namespace ASFW::Discovery
