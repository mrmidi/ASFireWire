// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceDeviceIo.hpp - Blocking DICE register access for the linear bring-up.
//
// Each call issues one asynchronous transaction through ProtocolRegisterIO (or
// one DICETransaction read, which reuses its parsers) and waits for it with
// DiceWaitClock::Backoff. The caller must not be on the queue that delivers
// bus completions (the driver's Default queue); if it were, the completion
// could never run. A per-transaction safety deadline turns that mistake into a
// logged kIOReturnTimeout instead of a hang.

#pragma once

#include "DICETransaction.hpp"
#include "DICETypes.hpp"
#include "DiceWaitClock.hpp"
#include "../../../../Protocols/Ports/ProtocolRegisterIO.hpp"

#include <DriverKit/IOReturn.h>

#include <atomic>
#include <cstdint>
#include <expected>
#include <vector>

namespace ASFW::Audio::DICE {

class DiceDeviceIo {
public:
    // Longest a single transaction may take before the wait gives up. The bus
    // layer times out on its own long before this; reaching it means the
    // completion could not be delivered.
    static constexpr uint32_t kSafetyDeadlineMs = 2000;

    DiceDeviceIo(Protocols::Ports::ProtocolRegisterIO& io,
                 DICETransaction& transaction,
                 DiceWaitClock& clock) noexcept
        : io_(io), transaction_(transaction), clock_(clock) {}

    // Offsets are relative to the DICE base (MakeDICEAddress).
    [[nodiscard]] std::expected<uint32_t, IOReturn> ReadQuad(uint32_t offset);
    [[nodiscard]] std::expected<std::vector<uint8_t>, IOReturn> ReadBlock(uint32_t offset,
                                                                         uint32_t length);
    [[nodiscard]] std::expected<void, IOReturn> WriteQuad(uint32_t offset, uint32_t value);
    // Returns the previous value; the swap happened only if it equals `expected`.
    [[nodiscard]] std::expected<uint64_t, IOReturn> CompareSwap64(uint32_t offset,
                                                                  uint64_t expected,
                                                                  uint64_t desired);

    [[nodiscard]] std::expected<GeneralSections, IOReturn> ReadGeneralSections();
    // Full GLOBAL section (reference-parity read size).
    [[nodiscard]] std::expected<GlobalState, IOReturn> ReadGlobalStateFull(const GeneralSections& sections);
    // Leading GLOBAL fields only.
    [[nodiscard]] std::expected<GlobalState, IOReturn> ReadGlobalState(const GeneralSections& sections);
    [[nodiscard]] std::expected<StreamConfig, IOReturn> ReadTxStreamConfig(const GeneralSections& sections);
    [[nodiscard]] std::expected<StreamConfig, IOReturn> ReadRxStreamConfig(const GeneralSections& sections);

    [[nodiscard]] Protocols::Ports::ProtocolRegisterIO& RegisterIo() noexcept { return io_; }
    [[nodiscard]] DiceWaitClock& Clock() noexcept { return clock_; }

private:
    // Wait until `done` is set by the completion, or the safety deadline passes.
    [[nodiscard]] bool AwaitCompletion(const std::atomic<bool>& done, const char* what);

    // Issue one asynchronous operation and wait for it (defined in the .cpp).
    template <typename T, typename Start>
    [[nodiscard]] std::expected<T, IOReturn> Run(const char* what, Start&& start);

    Protocols::Ports::ProtocolRegisterIO& io_;
    DICETransaction& transaction_;
    DiceWaitClock& clock_;
};

} // namespace ASFW::Audio::DICE
