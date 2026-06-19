// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerElection.hpp — IEEE 1394 Bus Manager election FSM (FW-18).

#pragma once

#include "../../Common/CSRSpace.hpp"
#include <cstdint>
#include <optional>

namespace ASFW::Bus {

enum class BmOwner : uint8_t {
    None,
    Local,
    Remote,
    Unknown
};

enum class DecisionAction : uint8_t {
    DoNotContend,
    ContendImmediately,
    ContendAfterGrace
};

enum class ElectionOutcome : uint8_t {
    WonBM,
    IncumbentReestablished,
    RemoteBM,
    ElectionFailed
};

struct BmElectionInputs {
    ASFW::FW::RoleMode mode;
    ASFW::FW::FullBMActivityLevel activityLevel;
    uint32_t generation;
    uint8_t localId;
    std::optional<uint8_t> irmId;
    bool wasIncumbent;
    bool abdicateObserved;
};

class BusManagerElection {
public:
    BusManagerElection() noexcept = default;
    ~BusManagerElection() = default;

    /**
     * @brief Decides the contender action for the new bus generation.
     */
    [[nodiscard]] DecisionAction Decide(const BmElectionInputs& inputs) noexcept;

    /**
     * @brief Interprets the raw old value returned from compare-swap.
     * Updates the inner state machine (current BM owner, etc.).
     */
    [[nodiscard]] ElectionOutcome InterpretOldValue(uint32_t oldValue, uint8_t localId) noexcept;

    /**
     * @brief Resets/aborts the FSM state, usually called on a bus reset.
     */
    void Reset() noexcept;

    // Accessors
    [[nodiscard]] BmOwner Owner() const noexcept { return owner_; }
    [[nodiscard]] std::optional<uint8_t> OwnerId() const noexcept { return ownerId_; }
    [[nodiscard]] uint32_t LastElectionGeneration() const noexcept { return lastElectionGen_; }
    [[nodiscard]] uint32_t StaleElectionAbortCount() const noexcept { return staleAbortCount_; }
    [[nodiscard]] uint32_t LastOldValue() const noexcept { return lastOldValue_; }

    void IncrementStaleAbortCount() noexcept { ++staleAbortCount_; }

private:
    BmOwner owner_{BmOwner::None};
    std::optional<uint8_t> ownerId_;
    uint32_t lastElectionGen_{0};
    uint32_t staleAbortCount_{0};
    uint32_t lastOldValue_{0x3F};
};

} // namespace ASFW::Bus
